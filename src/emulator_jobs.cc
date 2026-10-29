#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <format>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "absl/log/absl_log.h"
#include "googlesql/public/value.h"
#include "nlohmann/json.hpp"
#include "src/api_error.h"
#include "src/backend.h"
#include "src/duckdb_catalog.h"
#include "src/duckdb_sql.h"
#include "src/emulator.h"
#include "src/extract.h"
#include "src/extract_options.h"
#include "src/field_schema.h"
#include "src/file_read.h"
#include "src/file_read_options.h"
#include "src/gcs.h"
#include "src/gcs_uri.h"
#include "src/job_options.h"
#include "src/location.h"
#include "src/logging.h"
#include "src/parquet_metadata.h"
#include "src/references.h"
#include "src/result_googlesql.h"
#include "src/sql_runtime.h"
#include "src/table_metadata.h"
#include "src/temporary_files.h"
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

// An omitted decimal scale and an explicit zero name the same load type.
std::vector<FieldSchema> NormalizeLoadScales(std::vector<FieldSchema> schema) {
  for (auto& field : schema) {
    if (field.scale == 0) {
      field.scale.reset();
    }
    field.fields = NormalizeLoadScales(std::move(field.fields));
  }
  return schema;
}

// Whether a request that names `location`, or none, finds `job`: without a location, BigQuery
// finds a job only in the US or EU multi-region.
bool InLocation(const Job& job, const std::optional<std::string>& location) {
  if (!location.has_value()) {
    return job.location == "US" || job.location == "EU";
  }
  return CanonicalLocation(*location) == job.location;
}

// BigQuery's name for `operation`.
std::string_view OperationName(CopyOperation operation) {
  switch (operation) {
    case CopyOperation::kCopy:
      return "COPY";
    case CopyOperation::kSnapshot:
      return "SNAPSHOT";
    case CopyOperation::kRestore:
      return "RESTORE";
    case CopyOperation::kClone:
      return "CLONE";
  }
  return "";
}

// Rejects a source table of `copy` whose metadata is `metadata` that its operation does not take,
// with BigQuery's messages. What a CLONE of a snapshot makes, and which default rounding mode a
// SNAPSHOT, RESTORE or CLONE gives its table, are not known.
void CheckCopySource(const CopyJob& copy, const TableReference& source,
                     const TableMetadata& metadata) {
  const bool snapshot = metadata.snapshot.has_value();
  if (copy.operation == CopyOperation::kCopy && snapshot) {
    throw ApiError::Invalid("Copy operation on SNAPSHOT types is not allowed.");
  }
  if (copy.operation == CopyOperation::kRestore && !snapshot) {
    throw ApiError::Invalid(TableName(source) +
                            " is not allowed for this operation because it is currently a TABLE.");
  }
  const std::string what = "copy operationType " + std::string(OperationName(copy.operation));
  if (copy.operation == CopyOperation::kClone && snapshot) {
    throw ApiError::Unsupported(what + " of a table snapshot");
  }
  if (copy.operation != CopyOperation::kCopy &&
      metadata.default_rounding_mode != RoundingMode::kUnspecified) {
    throw ApiError::Unsupported(what + " of a table with a default rounding mode");
  }
}

// The metadata of the table that a SNAPSHOT, RESTORE or CLONE `copy` of `source`, whose metadata
// is `metadata`, creates: the source's without its expiration, with the source as the base table
// of a snapshot or a clone, as BigQuery answers. A RESTORE that replaces a table, which `exists`,
// keeps that table's metadata instead. Rejects WRITE_TRUNCATE for a SNAPSHOT with BigQuery's
// message, and the dispositions whose outcome is not known.
TableMetadata CopiedMetadata(const CopyJob& copy, const TableReference& source,
                             TableMetadata metadata, bool exists) {
  const std::string what = "copy operationType " + std::string(OperationName(copy.operation));
  const WriteDisposition write = copy.write_disposition;
  if (copy.operation == CopyOperation::kSnapshot && write == WriteDisposition::kWriteTruncate) {
    throw ApiError::Invalid(
        "Snapshot operation with write disposition WRITE_TRUNCATE is not supported. Please try "
        "again with WRITE_EMPTY.");
  }
  const bool replaces =
      copy.operation == CopyOperation::kRestore && write == WriteDisposition::kWriteTruncate;
  if ((write != WriteDisposition::kWriteEmpty && !replaces) || (exists && !replaces)) {
    throw ApiError::Unsupported(what + " with " + std::string(DispositionName(write)) +
                                (exists ? " to an existing table" : ""));
  }
  if (exists) {
    return {};
  }
  metadata.expiration_time.reset();
  metadata.creation_time = 0;
  metadata.clone.reset();
  metadata.snapshot.reset();
  if (copy.operation == CopyOperation::kRestore) {
    metadata.restored_from_snapshot = true;
  }
  if (copy.operation == CopyOperation::kSnapshot) {
    metadata.snapshot = BaseTableDefinition{.base_table = source};
  } else if (copy.operation == CopyOperation::kClone) {
    metadata.clone = BaseTableDefinition{.base_table = source};
  }
  return metadata;
}

}  // namespace

std::shared_ptr<const Job> Emulator::RunQuery(QueryRequest request) {
  request.project_id = ResolveProject(request.project_id);
  JobLocation location(RequestedLocation(request.location));
  auto job = std::make_shared<Job>();
  job->project_id = request.project_id;
  job->job_id = request.job_id;
  job->configuration = QueryJob{
      .query = request.query,
      .dry_run = request.dry_run,
      .destination_table = request.destination_table,
      .create_disposition = request.create_disposition,
      .write_disposition = request.write_disposition,
  };
  return RunJob(std::move(job), location, [&](Job& job) {
    job.result = ExecuteQuery(std::move(request), std::get<QueryJob>(job.configuration), location);
  });
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

  {
    const ScopedLogJob log_job(JobKey(job->project_id, job->job_id));
    if (job->query() != nullptr) {
      ABSL_VLOG(1) << "Query: " << job->query()->query;
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
    const int64_t elapsed = job->end_time_ms - job->creation_time_ms;
    if (!job->error.has_value()) {
      ABSL_LOG(INFO) << "Job done in " << elapsed << " ms";
    } else if (job->error->http_status() >= 500) {
      ABSL_LOG(ERROR) << "Job failed in " << elapsed << " ms: " << job->error->what();
    } else {
      ABSL_LOG(INFO) << "Job failed in " << elapsed << " ms: " << job->error->what();
    }
  }

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
  // BigQuery rejects a load into a table snapshot when the job is inserted, rather than failing
  // the job as it does a query's.
  try {
    TableReference destination = request.load.destination_table;
    destination.project_id = destination.project_id.empty()
                                 ? request.project_id
                                 : ResolveProject(destination.project_id);
    RejectReadOnlyWrite(GetTable(destination, /*include_row_count=*/false));
  } catch (const ApiError& error) {
    if (error.http_status() != 404) {
      throw;
    }
  }
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
    const FileFormat format = ParseLoadFormat(config);
    TemporaryFiles downloads;
    std::vector<FileSource> sources =
        StageFileSources(ParseLoadSourceUris(config), format, gcs_client_, downloads);
    // The destination's schema, or none when the load creates it.
    std::optional<std::vector<FieldSchema>> existing;
    try {
      existing = GetTable(load.destination_table, /*include_row_count=*/false).schema;
    } catch (const ApiError& error) {
      if (error.http_status() != 404) {
        throw;
      }
    }
    const bool appends =
        existing.has_value() && load.write_disposition != WriteDisposition::kWriteTruncate;
    std::vector<FieldSchema> requested_schema = SchemaFromJson(config.value("schema", json()));
    if (requested_schema.empty() && existing.has_value()) {
      requested_schema = *existing;
    }
    const LoadOptions options = ParseLoadOptions(format, config);
    // Map validated job options to reader settings; no destination policy crosses this boundary.
    FileReadOptions read_options = ParquetReadOptions{};
    if (const auto* csv = std::get_if<CsvLoadOptions>(&options)) {
      read_options = CsvReadOptions{
          .skip_leading_rows = csv->skip_leading_rows,
          .field_delimiter = csv->field_delimiter,
      };
    } else if (std::holds_alternative<JsonLoadOptions>(options)) {
      read_options = JsonReadOptions{};
    }
    ParquetColumn parquet;
    if (std::holds_alternative<JsonLoadOptions>(options)) {
      sources = StageJsonNumerics(sources, requested_schema, downloads);
    } else if (const auto* parquet_options = std::get_if<ParquetLoadOptions>(&options)) {
      ParquetSources staged =
          StageParquetSources(sources, downloads, parquet_options->enable_list_inference);
      sources = std::move(staged.sources);
      parquet = std::move(staged.columns);
      if (requested_schema.empty() &&
          (parquet.HasWideDecimal() ||
           std::ranges::any_of(parquet.children, [](const auto& column) {
             return column.edges != ParquetColumn::Edges::kNone;
           }))) {
        requested_schema = Prepare(FileReadQuery(read_options, sources, {}, parquet)).schema;
        DetectParquetSchema(requested_schema, parquet, ParseDecimalTargetTypes(config));
      }
    }
    auto read_schema = requested_schema;
    if (appends) {
      ReadUnrounded(read_schema, *existing);
    } else {
      for (auto& field : read_schema) {
        field = WithoutRoundingParameters(std::move(field));
      }
    }
    std::string sql = FileReadQuery(read_options, sources, read_schema, parquet);
    const QueryResult prepared = Prepare(sql);
    std::vector<FieldSchema> written =
        requested_schema.empty() ? prepared.schema : requested_schema;
    if (appends) {
      written = read_schema.empty() ? prepared.schema : read_schema;
    } else if (HasRoundHalfEven(written)) {
      std::vector<std::string> columns;
      for (size_t i = 0; i < written.size(); ++i) {
        const auto& field = written.at(i);
        const auto name = QuoteIdentifier(field.name);
        const auto rounded = TranslateAppendedValue(functions_, name, prepared.schema.at(i), field);
        if (!rounded) {
          throw ApiError::Unsupported("loading values into ROUND_HALF_EVEN columns");
        }
        columns.push_back(*rounded + " AS " + name);
      }
      sql = "SELECT " + Join(columns, ", ") + " FROM (" + sql + ") AS source";
    }
    if (existing.has_value() && !appends) {
      const auto info = GetTable(load.destination_table, /*include_row_count=*/false);
      const auto dataset = GetDataset({
          .project_id = load.destination_table.project_id,
          .dataset_id = load.destination_table.dataset_id,
      });
      if (info.metadata.default_collation.has_value() || dataset.default_collation.has_value()) {
        throw ApiError::Unsupported("load replacement involving a default collation");
      }
      auto inherited = written;
      ApplyDefaultRoundingMode(inherited, info.metadata.default_rounding_mode);
      // BigQuery keeps an unchanged schema, including its modes. A changed schema is replaced
      // as a whole with the load schema, without inheriting the retained table default.
      if (SameSchema(NormalizeLoadScales(inherited), NormalizeLoadScales(info.schema))) {
        written = info.schema;
      } else {
        SetModesGiven(written);
      }
    }
    // A table a load creates reports the mode of each field, given or detected.
    if (!existing.has_value()) {
      SetModesGiven(written);
    }
    const QueryResult result =
        WriteDestination(job.project_id, load.destination_table, load.create_disposition,
                         load.write_disposition, sql, written, {}, true);
    job.output_rows = result.rows.Int64(0, 0);
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
    if (copy.operation != CopyOperation::kCopy && copy.source_tables.size() > 1) {
      throw ApiError::Unsupported("copy operationType " +
                                  std::string(OperationName(copy.operation)) +
                                  " of several source tables");
    }
    std::vector<FieldSchema> schema;
    TableReference first_source;
    TableMetadata source_metadata;
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
      CheckCopySource(copy, source, table.metadata);
      if (copy.source_tables.size() > 1 && table.metadata.default_collation.has_value()) {
        throw ApiError::Unsupported("copying several source tables with a default collation");
      }
      if (!table.metadata.resource_tags.empty()) {
        throw ApiError::Unsupported("copy jobs from tables with tags");
      }
      if (sql.empty()) {
        schema = table.schema;
        first_source = source;
        source_metadata = table.metadata;
      } else if (!SameSchema(table.schema, schema)) {
        throw ApiError::Invalid("Source tables have different schemas");
      }
      sql += (sql.empty() ? "" : " UNION ALL ") + std::string("SELECT * FROM ") +
             ReadTableRelation(backend_, source);
    }
    // BigQuery cannot append a copy to a table whose additional columns have defaults
    // (third_party/bigquery_docs/default-values.md).
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
    // As BigQuery answers, a copy of one table takes its primary key, replacing a truncated
    // table's, while an appended table keeps its own and a copy of several tables takes none.
    const bool keyed = copy.source_tables.size() == 1 && !source_metadata.primary_key.empty();
    TableMetadata metadata;
    metadata.default_collation = source_metadata.default_collation;
    if (copy.operation != CopyOperation::kCopy || keyed) {
      bool exists = true;
      try {
        GetTable(copy.destination_table, /*include_row_count=*/false);
      } catch (const ApiError& error) {
        if (error.http_status() != 404) {
          throw;
        }
        exists = false;
      }
      if (copy.operation != CopyOperation::kCopy) {
        metadata = CopiedMetadata(copy, first_source, std::move(source_metadata), exists);
      } else if (!exists || copy.write_disposition == WriteDisposition::kWriteTruncate) {
        metadata.primary_key = source_metadata.primary_key;
      }
    }
    metadata.expiration_time = copy.destination_expiration_time;
    const QueryResult result =
        WriteDestination(job.project_id, copy.destination_table, copy.create_disposition,
                         copy.write_disposition, sql, schema, {}, true, metadata, /*copy=*/true,
                         /*restore=*/copy.operation == CopyOperation::kRestore);
    job.output_rows = result.rows.Int64(0, 0);
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

    const ExtractEncoding encoding = ParseExtractEncoding(extract.configuration);
    const bool parquet = encoding.format == FileFormat::kParquet;
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
                          ParquetExtractColumns(table.schema), ReadTableRelation(backend_, source),
                          QuoteLiteral(path),
                          encoding.compression == ExtractCompression::kNone
                              ? "uncompressed"
                              : ToLowerAscii(ExtractCompressionName(encoding.compression))));
      AnnotateParquetBigNumerics(path, table.schema);
    } else {
      const QueryResult result = Execute("SELECT * FROM " + ReadTableRelation(backend_, source));
      const TextExtractFormatOptions options =
          ParseTextExtractFormatOptions(encoding.format, extract.configuration);
      WriteTextExtract(
          table.schema, result.rows.size(),
          [&](size_t row, size_t column, const googlesql::Type* type) {
            return ReadGoogleSqlValue(result.rows, row, column, type);
          },
          options, encoding.compression, path);
    }
    if (uri.starts_with("gs://")) {
      gcs_client_.Upload(path, uri);
    }
    job.result = QueryResult{};
  });
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
