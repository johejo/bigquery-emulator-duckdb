#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <format>
#include <functional>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "nlohmann/json.hpp"
#include "src/api_error.h"
#include "src/backend.h"
#include "src/backend_error.h"
#include "src/ddl_write.h"
#include "src/duckdb_sql.h"
#include "src/emulator.h"
#include "src/extract.h"
#include "src/field_schema.h"
#include "src/gcs.h"
#include "src/load.h"
#include "src/location.h"
#include "src/parquet_metadata.h"
#include "src/references.h"
#include "src/schema_sql.h"
#include "src/table_metadata.h"
#include "src/temporary_files.h"
#include "src/translated_statement.h"
#include "src/translator.h"

namespace bigquery_emulator_duckdb {
namespace {

using nlohmann::json;

int64_t NowMillis() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::string JobKey(const std::string& project_id, const std::string& job_id) {
  return project_id + ":" + job_id;
}

// The field of `schema` named `name`, which BigQuery compares without case.
std::vector<FieldSchema>::const_iterator FindField(const std::vector<FieldSchema>& schema,
                                                   const std::string& name) {
  return std::ranges::find_if(schema, [&](const FieldSchema& field) {
    return std::ranges::equal(field.name, name, [](unsigned char a, unsigned char b) {
      return std::tolower(a) == std::tolower(b);
    });
  });
}

// Removes the precision and scale of the fields of `schema` that `destination` rounds to even, so
// that a load reads their values unrounded and WriteDestination rounds them.
void ReadUnrounded(std::vector<FieldSchema>& schema, const std::vector<FieldSchema>& destination) {
  for (FieldSchema& field : schema) {
    const auto column = FindField(destination, field.name);
    if (column == destination.end()) {
      continue;
    }
    if (column->rounding_mode == kRoundHalfEven && column->precision.has_value()) {
      field.precision.reset();
      field.scale.reset();
    }
    ReadUnrounded(field.fields, column->fields);
  }
}

// Whether a request that names `location`, or none, finds `job`: without a location, BigQuery
// finds a job only in the US or EU multi-region.
bool InLocation(const Job& job, const std::optional<std::string>& location) {
  if (!location.has_value()) {
    return job.location == "US" || job.location == "EU";
  }
  return CanonicalLocation(*location) == job.location;
}

}  // namespace

void Emulator::UseDataset(JobLocation& location, const TableReference& table) {
  const DatasetReference dataset{.project_id = table.project_id, .dataset_id = table.dataset_id};
  if (const std::optional<std::string> found = DatasetLocation(backend_, dataset)) {
    location.Reference("Dataset " + dataset.project_id + ":" + dataset.dataset_id, *found);
  }
}

std::string_view DispositionName(CreateDisposition disposition) {
  switch (disposition) {
    case CreateDisposition::kCreateIfNeeded:
      return "CREATE_IF_NEEDED";
    case CreateDisposition::kCreateNever:
      return "CREATE_NEVER";
  }
  return "";
}

std::string_view DispositionName(WriteDisposition disposition) {
  switch (disposition) {
    case WriteDisposition::kWriteEmpty:
      return "WRITE_EMPTY";
    case WriteDisposition::kWriteAppend:
      return "WRITE_APPEND";
    case WriteDisposition::kWriteTruncate:
      return "WRITE_TRUNCATE";
    case WriteDisposition::kWriteTruncateData:
      return "WRITE_TRUNCATE_DATA";
  }
  return "";
}

std::optional<CreateDisposition> ParseCreateDisposition(std::string_view name) {
  for (const CreateDisposition disposition :
       {CreateDisposition::kCreateIfNeeded, CreateDisposition::kCreateNever}) {
    if (DispositionName(disposition) == name) {
      return disposition;
    }
  }
  return std::nullopt;
}

std::optional<WriteDisposition> ParseWriteDisposition(std::string_view name) {
  for (const WriteDisposition disposition : {
           WriteDisposition::kWriteEmpty,
           WriteDisposition::kWriteAppend,
           WriteDisposition::kWriteTruncate,
           WriteDisposition::kWriteTruncateData,
       }) {
    if (DispositionName(disposition) == name) {
      return disposition;
    }
  }
  return std::nullopt;
}

std::shared_ptr<const Job> Emulator::RunJob(std::shared_ptr<Job> job, JobLocation& location,
                                            const std::function<void(Job&)>& body) {
  job->creation_time_ms = NowMillis();
  {
    std::scoped_lock const lock(mutex_);
    if (job->job_id.empty()) {
      while (job->job_id.empty() || jobs_.contains(JobKey(job->project_id, job->job_id)) ||
             running_jobs_.contains(JobKey(job->project_id, job->job_id))) {
        job->job_id = "job_" + std::to_string(next_job_number_++);
      }
    }
    if (!job->dry_run()) {
      const std::string key = JobKey(job->project_id, job->job_id);
      if (jobs_.contains(key) || running_jobs_.contains(key)) {
        throw ApiError::Duplicate("Already Exists: Job " + key);
      }
      running_jobs_.insert(key);
    }
  }

  try {
    body(*job);
  } catch (const ApiError& error) {
    job->error = error;
  } catch (const std::exception& error) {
    job->error = job->query() != nullptr ? ApiError::InvalidQuery(error.what())
                                         : ApiError::Invalid(error.what());
  }
  job->location = location.Resolve();
  job->end_time_ms = NowMillis();

  if (job->dry_run()) {
    return job;
  }
  std::scoped_lock const lock(mutex_);
  const std::string key = JobKey(job->project_id, job->job_id);
  running_jobs_.erase(key);
  jobs_[key] = job;
  return job;
}

std::shared_ptr<const Job> Emulator::RunLoad(LoadRequest request) {
  request.project_id = ResolveProject(request.project_id);
  JobLocation location(RequestedLocation(request.location));
  auto job = std::make_shared<Job>();
  job->project_id = request.project_id;
  job->job_id = request.job_id;
  job->configuration = request.load;
  return RunJob(std::move(job), location, [&](Job& job) {
    auto& load = std::get<LoadJob>(job.configuration);
    load.destination_table.project_id = ResolveProject(load.destination_table.project_id.empty()
                                                           ? request.project_id
                                                           : load.destination_table.project_id);
    load.configuration["destinationTable"]["projectId"] = load.destination_table.project_id;
    UseDataset(location, load.destination_table);

    const json& config = load.configuration;
    const std::string format = config.value("sourceFormat", "CSV");
    if (format != "CSV" && format != "NEWLINE_DELIMITED_JSON" && format != "PARQUET") {
      throw ApiError::Invalid("Unsupported source format: " + format);
    }
    TemporaryFiles downloads;
    std::vector<std::string> paths = StageLoadSources(config, format, gcs_client_, downloads);
    // The destination's schema, or none when the load creates it.
    std::optional<std::vector<FieldSchema>> existing;
    try {
      existing = GetTable(load.destination_table, /*include_row_count=*/false).schema;
    } catch (const ApiError& error) {
      if (error.http_status() != 404) {
        throw;
      }
    }
    // Values a load appends to ROUND_HALF_EVEN fields are read unrounded and rounded to even as
    // WriteDestination writes them. A table the load creates takes its dataset's default rounding
    // mode, but BigQuery stores 2.25 as 2.3 in a NUMERIC(5, 1) field that takes ROUND_HALF_EVEN
    // from it, so loads that create or replace such fields are unsupported.
    const bool appends =
        existing.has_value() && load.write_disposition != WriteDisposition::kWriteTruncate;
    const auto check_rounding = [&](std::vector<FieldSchema> written) {
      if (appends) {
        return;
      }
      if (!existing.has_value()) {
        ApplyDefaultRoundingMode(written,
                                 GetDataset(DatasetReference{
                                                .project_id = load.destination_table.project_id,
                                                .dataset_id = load.destination_table.dataset_id,
                                            })
                                     .default_rounding_mode);
      }
      if (HasRoundHalfEven(written)) {
        throw ApiError::Unsupported("load jobs that create or replace ROUND_HALF_EVEN columns");
      }
    };
    std::vector<FieldSchema> requested_schema = SchemaFromJson(config.value("schema", json()));
    if (requested_schema.empty() && existing.has_value()) {
      requested_schema = *existing;
    }
    ParquetColumn parquet;
    if (format == "NEWLINE_DELIMITED_JSON") {
      paths = StageJsonNumerics(paths, requested_schema, downloads);
    } else if (format == "PARQUET") {
      const json options = config.value("parquetOptions", json::object());
      ParquetSources sources =
          StageParquetDecimals(paths, downloads, options.value("enableListInference", false));
      paths = std::move(sources.paths);
      parquet = std::move(sources.columns);
      if (requested_schema.empty() && parquet.HasWideDecimal()) {
        requested_schema = Prepare(LoadQuery(format, paths, config, {})).schema;
        DetectParquetDecimals(requested_schema, parquet, config);
      }
    }
    check_rounding(requested_schema);
    if (appends) {
      ReadUnrounded(requested_schema, *existing);
    }
    const std::string sql = LoadQuery(format, paths, config, requested_schema, parquet);
    const QueryResult prepared = Prepare(sql);
    if (requested_schema.empty()) {
      check_rounding(prepared.schema);
    }
    std::vector<FieldSchema> written =
        requested_schema.empty() ? prepared.schema : requested_schema;
    // A table a load creates reports the mode of each field, given or detected.
    if (!existing.has_value()) {
      SetModesGiven(written);
    }
    const QueryResult result =
        WriteDestination(job.project_id, load.destination_table, load.create_disposition,
                         load.write_disposition, sql, written, {}, true);
    job.output_rows = std::stoll(result.rows.at(0).at("f").at(0).at("v").get<std::string>());
    job.result = QueryResult{};
  });
}

std::shared_ptr<const Job> Emulator::RunCopy(CopyRequest request) {
  request.project_id = ResolveProject(request.project_id);
  JobLocation location(RequestedLocation(request.location));
  auto job = std::make_shared<Job>();
  job->project_id = request.project_id;
  job->job_id = request.job_id;
  job->configuration = request.copy;
  return RunJob(std::move(job), location, [&](Job& job) {
    auto& copy = std::get<CopyJob>(job.configuration);
    for (TableReference& source : copy.source_tables) {
      source.project_id =
          ResolveProject(source.project_id.empty() ? request.project_id : source.project_id);
    }
    copy.destination_table.project_id = ResolveProject(copy.destination_table.project_id.empty()
                                                           ? request.project_id
                                                           : copy.destination_table.project_id);
    copy.configuration["destinationTable"]["projectId"] = copy.destination_table.project_id;
    if (!copy.source_tables.empty() && copy.configuration.contains("sourceTable")) {
      copy.configuration["sourceTable"]["projectId"] = copy.source_tables.front().project_id;
    }
    if (copy.configuration.contains("sourceTables")) {
      for (size_t i = 0; i < copy.source_tables.size(); ++i) {
        copy.configuration["sourceTables"][i]["projectId"] = copy.source_tables.at(i).project_id;
      }
    }

    if (copy.source_tables.empty()) {
      throw ApiError::Invalid("Source table is required");
    }
    for (const TableReference& source : copy.source_tables) {
      UseDataset(location, source);
    }
    // BigQuery copies tables across locations as well, which the emulator does not.
    if (const std::optional<std::string> destination =
            DatasetLocation(backend_,
                            {
                                .project_id = copy.destination_table.project_id,
                                .dataset_id = copy.destination_table.dataset_id,
                            });
        destination.has_value() && *destination != location.Resolve()) {
      throw ApiError::Unsupported("copying tables to a dataset in another location");
    }
    std::vector<FieldSchema> schema;
    std::string sql;
    for (TableReference source : copy.source_tables) {
      if (source.project_id.empty()) {
        source.project_id = job.project_id;
      }
      source.project_id = ResolveProject(source.project_id);
      const TableInfo table = GetTable(source);
      if (table.view_query) {
        throw ApiError::Invalid("Cannot copy a view: " + TableName(source));
      }
      if (sql.empty()) {
        schema = table.schema;
      } else if (!SameSchema(table.schema, schema)) {
        throw ApiError::Invalid("Source tables have different schemas");
      }
      sql += (sql.empty() ? "" : " UNION ALL ") + std::string("SELECT * FROM ") +
             QualifiedName(source);
    }
    // BigQuery cannot append a copy to a table whose additional columns have defaults
    // (https://cloud.google.com/bigquery/docs/default-values#limitations).
    if (copy.write_disposition == WriteDisposition::kWriteAppend) {
      std::vector<FieldSchema> destination;
      try {
        destination = GetTable(copy.destination_table).schema;
      } catch (const ApiError& error) {
        if (error.http_status() != 404) {
          throw;
        }
      }
      for (const FieldSchema& field : destination) {
        const bool copied = std::ranges::any_of(schema, [&](const FieldSchema& column) {
          return ToLowerAscii(column.name) == ToLowerAscii(field.name);
        });
        if (!copied && !field.default_value_expression.empty()) {
          throw ApiError::Invalid("Cannot append a copy to " + TableName(copy.destination_table) +
                                  ": column " + field.name +
                                  " has a default value and is not in the source table");
        }
      }
    }
    TableMetadata metadata;
    metadata.expiration_time = copy.destination_expiration_time;
    const QueryResult result =
        WriteDestination(job.project_id, copy.destination_table, copy.create_disposition,
                         copy.write_disposition, sql, schema, {}, true, metadata, /*copy=*/true);
    job.output_rows = std::stoll(result.rows.at(0).at("f").at(0).at("v").get<std::string>());
    job.result = QueryResult{};
  });
}

std::shared_ptr<const Job> Emulator::RunExtract(ExtractRequest request) {
  request.project_id = ResolveProject(request.project_id);
  JobLocation location(RequestedLocation(request.location));
  auto job = std::make_shared<Job>();
  job->project_id = request.project_id;
  job->job_id = request.job_id;
  job->configuration = request.extract;
  return RunJob(std::move(job), location, [&](Job& job) {
    auto& extract = std::get<ExtractJob>(job.configuration);
    extract.source_table.project_id =
        ResolveProject(extract.source_table.project_id.empty() ? request.project_id
                                                               : extract.source_table.project_id);
    extract.configuration["sourceTable"]["projectId"] = extract.source_table.project_id;
    UseDataset(location, extract.source_table);

    const json& config = extract.configuration;
    const std::string format = config.value("destinationFormat", "CSV");
    const std::string compression = config.value("compression", "NONE");
    if (format == "AVRO") {
      throw ApiError::Unsupported("Avro extracts");
    }
    if (format != "CSV" && format != "NEWLINE_DELIMITED_JSON" && format != "PARQUET") {
      throw ApiError::Invalid("Unsupported destination format: " + format);
    }
    const bool parquet = format == "PARQUET";
    if (compression != "NONE" && compression != "GZIP" &&
        !(parquet && (compression == "SNAPPY" || compression == "ZSTD"))) {
      throw ApiError::Invalid("Unsupported compression " + compression + " for " + format);
    }
    if (extract.destination_uris.size() != 1) {
      throw ApiError::Unsupported("multiple destination URIs");
    }
    std::string uri = extract.destination_uris.front();
    TemporaryFiles uploads;
    std::string path;
    if (uri.starts_with("gs://")) {
      // The whole table fits in the first file of a wildcard URI's sequence.
      if (const size_t wildcard = FindGcsWildcard(uri); wildcard != std::string::npos) {
        uri.replace(wildcard, 1, "000000000000");
      }
      path = uploads.Create();
    } else if (uri.starts_with("file://")) {
      path = uri.substr(7);
    } else if (uri.find("://") == std::string::npos) {
      path = uri;
    } else {
      throw ApiError::Invalid("Unsupported destination URI: " + uri);
    }

    TableReference source = extract.source_table;
    if (source.project_id.empty()) {
      source.project_id = job.project_id;
    }
    source.project_id = ResolveProject(source.project_id);
    const TableInfo table = GetTable(source);
    if (table.view_query) {
      throw ApiError::Invalid("Cannot extract a view: " + TableName(source));
    }
    if (parquet) {
      Execute(std::format("COPY (SELECT {} FROM {}) TO {} (FORMAT parquet, COMPRESSION {})",
                          ParquetExtractColumns(table.schema), QualifiedName(source),
                          QuoteLiteral(path),
                          compression == "NONE" ? "uncompressed" : ToLowerAscii(compression)));
      AnnotateParquetBigNumerics(path, table.schema);
    } else {
      WriteTextExtract(table.schema, Execute("SELECT * FROM " + QualifiedName(source)).rows,
                       {
                           .json = format == "NEWLINE_DELIMITED_JSON",
                           .gzip = compression == "GZIP",
                           .field_delimiter = config.value("fieldDelimiter", ","),
                           .print_header = config.value("printHeader", true),
                       },
                       path);
    }
    if (uri.starts_with("gs://")) {
      gcs_client_.Upload(path, uri);
    }
    job.result = QueryResult{};
  });
}

QueryResult Emulator::WriteDestination(const std::string& project_id, TableReference destination,
                                       CreateDisposition create, WriteDisposition write,
                                       const std::string& sql,
                                       const std::vector<FieldSchema>& schema,
                                       const std::vector<std::string>& setup, bool count_only,
                                       const TableMetadata& metadata, bool copy) {
  std::set<std::string> names;
  std::vector<std::string> duplicates;
  for (const FieldSchema& field : schema) {
    std::string name = field.name;
    std::ranges::transform(name, name.begin(), [](unsigned char c) { return std::tolower(c); });
    if (!names.insert(name).second) {
      duplicates.push_back(field.name);
    }
  }
  if (!duplicates.empty()) {
    throw ApiError::InvalidQuery(
        "Duplicate column names in the result are not supported. Found duplicate(s): " +
        Join(duplicates, ", "));
  }

  if (destination.project_id.empty()) {
    destination.project_id = project_id;
  }
  destination.project_id = ResolveProject(destination.project_id);
  const DatasetMetadata dataset = GetDataset(
      DatasetReference{.project_id = destination.project_id, .dataset_id = destination.dataset_id});
  std::optional<TableInfo> existing;
  try {
    existing = GetTable(destination);
  } catch (const ApiError& error) {
    if (error.http_status() != 404) {
      throw;
    }
  }
  if (existing.has_value() && existing->view_query) {
    throw ApiError::Invalid("Cannot write to a view: " + TableName(destination));
  }
  if (!existing.has_value() && create == CreateDisposition::kCreateNever) {
    throw ApiError::NotFound("Not found: Table " + TableName(destination));
  }
  if (existing.has_value() && write == WriteDisposition::kWriteEmpty && existing->num_rows > 0) {
    throw ApiError::Duplicate("Already Exists: Table " + TableName(destination));
  }
  // Appended rows are written with DuckDB's casts, which round halfway values away from zero, so
  // the values of ROUND_HALF_EVEN fields are rounded to even first, as BigQuery rounds those a
  // query or a load appends. A copy from a table of the same schema has nothing to round, and
  // BigQuery rejects a copy from another.
  std::string appended = "*";
  if (existing.has_value() && write != WriteDisposition::kWriteTruncate &&
      HasRoundHalfEven(existing->schema) && !SameSchema(existing->schema, schema)) {
    std::vector<std::string> columns;
    for (const FieldSchema& field : schema) {
      const std::string name = QuoteIdentifier(field.name);
      const auto column = FindField(existing->schema, field.name);
      const std::optional<std::string> value =
          column == existing->schema.end() ? name : TranslateAppendedValue(name, field, *column);
      if (copy || !value.has_value()) {
        throw ApiError::Unsupported("appending rows of another schema to ROUND_HALF_EVEN columns");
      }
      columns.push_back(*value + " AS " + name);
    }
    appended = Join(columns, ", ");
  }
  // Whether BigQuery replaces an existing table's metadata, or keeps it, is not known.
  if (existing.has_value() && !metadata.empty()) {
    throw ApiError::Unsupported("setting the expiration of an existing destination table " +
                                TableName(destination));
  }

  // The result is materialized first, which runs the query once even when it reads the
  // destination itself. DuckDB lets a transaction write to a single database, so the temporary
  // table is filled before the transaction that writes the destination begins.
  const std::string result_table = "temp.main._bigquery_emulator_query_result";
  std::vector<std::string> aliases;
  std::ranges::transform(schema, std::back_inserter(aliases),
                         [&](const FieldSchema& field) { return QuoteIdentifier(field.name); });
  const std::string target = QualifiedName(destination);
  std::vector<std::string> statements = {
      std::format("CREATE TEMP TABLE _bigquery_emulator_query_result AS"
                  " SELECT * FROM ({}) AS _bigquery_emulator_query_result({})",
                  sql, Join(aliases, ", ")),
      "BEGIN TRANSACTION",
  };
  if (!existing.has_value() || write == WriteDisposition::kWriteTruncate) {
    if (existing.has_value()) {
      statements.push_back("DROP TABLE " + target);
    }
    // The columns take BigQuery's types for the result, so the table reads back as the query's
    // schema rather than as whatever DuckDB computed.
    if (const std::optional<std::string> columns = ColumnDefinitions(schema)) {
      statements.push_back(std::format("CREATE TABLE {} ({})", target, *columns));
    } else {
      statements.push_back("CREATE TABLE " + target + " AS SELECT * FROM " + result_table +
                           " LIMIT 0");
    }
    // Creation and its schema, defaults and table metadata commit together, as for DDL and REST.
    // The job's disposition decides what to replace; CreateTableWrite only initializes the table.
    // Only a table the job creates takes the dataset's default expiration, which BigQuery
    // documents for new tables. As BigQuery answers, a replaced table keeps its default rounding
    // mode, which its new fields do not take, and a copy job's new table takes no default rounding
    // mode, neither the source's nor the dataset's.
    TableDefinition definition{
        .table = destination,
        .schema = schema,
        .metadata = metadata,
        .keeps_rounding_modes = existing.has_value() || copy,
    };
    DatasetMetadata defaults = existing.has_value() ? DatasetMetadata{} : dataset;
    if (existing.has_value()) {
      definition.metadata.default_rounding_mode = existing->metadata.default_rounding_mode;
    } else if (copy) {
      defaults.default_rounding_mode.clear();
    }
    std::ranges::move(CreateTableWrite(definition, defaults).metadata_statements,
                      std::back_inserter(statements));
    std::ranges::move(ColumnDefaultStatements(destination, schema), std::back_inserter(statements));
    statements.push_back("INSERT INTO " + target + " SELECT * FROM " + result_table);
  } else {
    if (write == WriteDisposition::kWriteTruncateData) {
      statements.push_back("DELETE FROM " + target);
    }
    statements.push_back("INSERT INTO " + target + " BY NAME SELECT " + appended + " FROM " +
                         result_table);
  }
  statements.emplace_back("COMMIT");
  statements.push_back("SELECT " + std::string(count_only ? "count(*)" : "*") + " FROM " +
                       result_table);
  try {
    return backend_.ExecuteAll(statements, setup);
  } catch (const BackendError& error) {
    throw ApiError::InvalidQuery(error.what());
  }
}

std::shared_ptr<const Job> Emulator::GetJob(std::string project_id, const std::string& job_id,
                                            const std::optional<std::string>& location) {
  project_id = ResolveProject(project_id);
  std::scoped_lock const lock(mutex_);
  const auto it = jobs_.find(JobKey(project_id, job_id));
  if (it == jobs_.end() || !InLocation(*it->second, location)) {
    throw ApiError::NotFound("Not found: Job " + project_id + ":" + job_id);
  }
  return it->second;
}

std::vector<std::shared_ptr<const Job>> Emulator::ListJobs(std::string project_id) {
  project_id = ResolveProject(project_id);
  std::scoped_lock const lock(mutex_);
  std::vector<std::shared_ptr<const Job>> result;
  for (const auto& entry : jobs_) {
    const auto& job = entry.second;
    if (job->project_id == project_id) {
      result.push_back(job);
    }
  }
  std::ranges::sort(result, [](const auto& left, const auto& right) {
    if (left->creation_time_ms != right->creation_time_ms) {
      return left->creation_time_ms > right->creation_time_ms;
    }
    return left->job_id > right->job_id;
  });
  return result;
}

void Emulator::DeleteJob(std::string project_id, const std::string& job_id,
                         const std::optional<std::string>& location) {
  project_id = ResolveProject(project_id);
  std::scoped_lock const lock(mutex_);
  const auto it = jobs_.find(JobKey(project_id, job_id));
  if (it == jobs_.end() || !InLocation(*it->second, location)) {
    throw ApiError::NotFound("Not found: Job " + project_id + ":" + job_id);
  }
  jobs_.erase(it);
}

}  // namespace bigquery_emulator_duckdb
