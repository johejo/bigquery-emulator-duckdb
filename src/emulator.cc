#include "src/emulator.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/status/statusor.h"
#include "googlesql/public/strings.h"
#include "googlesql/public/type.h"
#include "googlesql/resolved_ast/resolved_ast.h"
#include "nlohmann/json.hpp"
#include "src/analyzer.h"
#include "src/api_error.h"
#include "src/backend.h"
#include "src/catalog.h"
#include "src/column_metadata.h"
#include "src/ddl_write.h"
#include "src/duckdb_sql.h"
#include "src/extract.h"
#include "src/field_schema.h"
#include "src/gcs.h"
#include "src/load.h"
#include "src/references.h"
#include "src/schema_sql.h"
#include "src/table_comments.h"
#include "src/table_metadata.h"
#include "src/temporary_files.h"
#include "src/translator.h"
#include "src/type_mapping.h"

namespace bigquery_emulator_duckdb {
namespace {

using nlohmann::json;

int64_t NowMillis() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// The file a project is stored in. Project ids may carry a domain ("example.com:project"), so
// every byte outside [A-Za-z0-9_-] is percent-encoded. With '.' and '/' encoded, no id can name a
// path outside the data directory.
std::string ProjectFileName(const std::string& project_id) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  std::string name;
  for (const char c : project_id) {
    const auto byte = static_cast<unsigned char>(c);
    if (std::isalnum(byte) != 0 || c == '_' || c == '-') {
      name += c;
    } else {
      name += '%';
      name += kHex[byte >> 4];
      name += kHex[byte & 0xF];
    }
  }
  return name + ".duckdb";
}

std::string JobKey(const std::string& project_id, const std::string& job_id) {
  return project_id + ":" + job_id;
}

std::vector<std::string> FirstColumnStrings(const QueryResult& result) {
  std::vector<std::string> values;
  values.reserve(result.rows.size());
  std::ranges::transform(result.rows, std::back_inserter(values),
                         [](const json& row) { return row["f"][0]["v"].get<std::string>(); });
  return values;
}

// DuckDB's own schemas, which are not datasets. `main` holds what the emulator records about
// the project's datasets; see DatasetMetadataTable.
constexpr std::string_view kDuckDbSchemas = "('main', 'information_schema', 'pg_catalog')";

bool IsDuckDbSchema(const std::string& name) {
  return name == "main" || name == "information_schema" || name == "pg_catalog";
}

// The datasets of `project`, by name.
std::string DatasetsQuery(const std::string& project) {
  return std::format(
      "SELECT schema_name FROM information_schema.schemata WHERE catalog_name = {}"
      " AND schema_name NOT IN {} ORDER BY schema_name",
      QuoteLiteral(project), kDuckDbSchemas);
}

// The datasets of `project` by name, each with the metadata recorded for it, or only `dataset`.
std::string DatasetEntriesQuery(const std::string& project,
                                const std::optional<std::string>& dataset = std::nullopt) {
  return std::format(
      "SELECT schema_name, (SELECT metadata FROM {} WHERE dataset_id = schema_name)"
      " FROM information_schema.schemata WHERE catalog_name = {} AND schema_name NOT IN {}{}"
      " ORDER BY schema_name",
      DatasetMetadataTable(project), QuoteLiteral(project), kDuckDbSchemas,
      dataset.has_value() ? " AND schema_name = " + QuoteLiteral(*dataset) : "");
}

DatasetMetadata ParseDatasetMetadata(const json& recorded) {
  if (!recorded.is_string()) {
    return {};
  }
  return DatasetMetadataFromJson(json::parse(recorded.get<std::string>()));
}

// The tables and views of `dataset` by name.
std::string TablesQuery(const DatasetReference& dataset) {
  return std::format(
      "SELECT table_name FROM information_schema.tables WHERE table_catalog = {}"
      " AND table_schema = {} ORDER BY table_name",
      QuoteLiteral(dataset.project_id), QuoteLiteral(dataset.dataset_id));
}

// The comment of `table` in `relations`, duckdb_tables() or duckdb_views(), whose `name` column
// names it. The comment is NULL when it has none, and nothing is returned when there is no such
// table or view.
std::optional<json> RelationComment(Backend& backend, std::string_view relations,
                                    std::string_view name, const TableReference& table) {
  const QueryResult result = backend.Execute(std::format(
      "SELECT comment FROM {} WHERE database_name = {} AND schema_name = {} AND {} = {}", relations,
      QuoteLiteral(table.project_id), QuoteLiteral(table.dataset_id), name,
      QuoteLiteral(table.table_id)));
  if (result.rows.empty()) {
    return std::nullopt;
  }
  return result.rows[0]["f"][0]["v"];
}

std::optional<json> ViewComment(Backend& backend, const TableReference& table) {
  return RelationComment(backend, "duckdb_views()", "view_name", table);
}

// The BigQuery schema of `table`: its columns as DuckDB types describe them, each replaced by the
// field its comment records. Throws BackendError when the table does not exist.
std::vector<FieldSchema> TableSchema(Backend& backend, const TableReference& table) {
  std::vector<FieldSchema> schema = backend.Prepare("SELECT * FROM " + QualifiedName(table)).schema;
  std::vector<json> comments;
  std::ranges::transform(backend.Execute(ColumnCommentsQuery(table)).rows,
                         std::back_inserter(comments),
                         [](const json& row) { return row["f"][0]["v"]; });
  return ApplyColumnComments(std::move(schema), comments);
}

// Queries see a GEOGRAPHY column as the STRING it is stored as, since the emulator does not
// translate GEOGRAPHY in SQL; tables.get still reports the column as GEOGRAPHY.
void GeographyAsString(std::vector<FieldSchema>& schema) {
  for (FieldSchema& field : schema) {
    if (field.type == FieldType::kGeography) {
      field.type = FieldType::kString;
    }
    GeographyAsString(field.fields);
  }
}

// Serves the analyzer the tables the emulator keeps in DuckDB.
class DuckDbTableSource : public TableSource {
 public:
  explicit DuckDbTableSource(Backend& backend) : backend_(backend) {}

  std::optional<std::vector<FieldSchema>> FindTable(const std::string& project,
                                                    const std::string& dataset,
                                                    const std::string& table) override {
    if (IsDuckDbSchema(dataset)) {
      return std::nullopt;
    }
    try {
      std::vector<FieldSchema> schema =
          TableSchema(backend_, TableReference{project, dataset, table});
      GeographyAsString(schema);
      return schema;
    } catch (const BackendError&) {
      return std::nullopt;
    }
  }

  std::optional<TableDescription> DescribeTable(const std::string& project,
                                                const std::string& dataset,
                                                const std::string& table) override {
    const TableReference reference{project, dataset, table};
    if (IsDuckDbSchema(dataset)) {
      return std::nullopt;
    }
    try {
      const std::optional<json> comment =
          RelationComment(backend_, "duckdb_tables()", "table_name", reference);
      if (!comment.has_value()) {
        return std::nullopt;
      }
      return TableDescription{.schema = TableSchema(backend_, reference),
                              .metadata = CommentMetadata(*comment)};
    } catch (const BackendError&) {
      return std::nullopt;
    }
  }

  std::vector<std::string> ListDatasets(const std::string& project) override {
    return FirstColumnStrings(backend_.Execute(DatasetsQuery(project)));
  }

  std::vector<std::string> ListTables(const std::string& project,
                                      const std::string& dataset) override {
    return FirstColumnStrings(backend_.Execute(TablesQuery(DatasetReference{project, dataset})));
  }

  std::optional<std::string> FindViewQuery(const std::string& project, const std::string& dataset,
                                           const std::string& table) override {
    const std::optional<json> comment =
        ViewComment(backend_, TableReference{project, dataset, table});
    if (!comment.has_value()) {
      return std::nullopt;
    }
    const std::optional<ViewMetadata> metadata = ParseViewMetadata(*comment);
    return metadata.has_value() ? metadata->query : "";
  }

 private:
  Backend& backend_;
};

// Rejects DDL whose target is in one of DuckDB's own schemas: DDL names its target by path, so
// such a path reaches past the datasets.
void CheckDdlTarget(const TranslatedStatement& translation) {
  if (const auto& target = translation.ddl_target_table;
      target.has_value() && IsDuckDbSchema(target->dataset_id)) {
    throw ApiError::NotFound("Not found: Dataset " + target->project_id + ":" + target->dataset_id);
  }
  if (const auto& target = translation.ddl_target_dataset;
      target.has_value() && IsDuckDbSchema(target->dataset_id)) {
    throw ApiError::NotFound("Not found: Dataset " + target->project_id + ":" + target->dataset_id);
  }
}

const googlesql::Type* ParameterType(const FieldSchema& field,
                                     googlesql::TypeFactory& type_factory) {
  absl::StatusOr<const googlesql::Type*> type = GoogleSqlType(field, &type_factory);
  if (!type.ok()) {
    throw ApiError::InvalidQuery(std::string(type.status().message()));
  }
  return *type;
}

// Types whose values the backend writes the same way on the wire, so that a column DuckDB
// computed as one of them can be reported as another: all numbers are decimal strings, and
// these textual types are plain strings.
bool SameWireEncoding(FieldType a, FieldType b) {
  static const auto* const kGroups = new std::vector<std::set<FieldType>>{
      {FieldType::kInteger, FieldType::kFloat, FieldType::kNumeric, FieldType::kBigNumeric},
      {FieldType::kString, FieldType::kJson, FieldType::kGeography}};
  if (a == b) {
    return true;
  }
  return std::ranges::any_of(*kGroups, [&](const std::set<FieldType>& group) {
    return group.contains(a) && group.contains(b);
  });
}

// Takes the column names, and the types where it can, from the schema the analyzer resolved,
// which follows BigQuery's typing rules (SUM of INT64 is INT64, an unnamed column is f0_), over
// the one DuckDB reports. The rows are still encoded after DuckDB's types, so a type that
// would change their encoding (DATETIME for a TIMESTAMP, say) is left as DuckDB has it.
std::vector<FieldSchema> ReconcileSchema(std::vector<FieldSchema> duckdb_schema,
                                         const std::vector<FieldSchema>& resolved_schema) {
  if (duckdb_schema.size() != resolved_schema.size()) {
    return duckdb_schema;
  }
  for (size_t i = 0; i < duckdb_schema.size(); ++i) {
    FieldSchema& field = duckdb_schema[i];
    const FieldSchema& resolved = resolved_schema[i];
    field.name = resolved.name;
    if (field.mode != resolved.mode) {
      continue;
    }
    if (field.type == FieldType::kRecord && resolved.type == FieldType::kRecord) {
      field.fields = ReconcileSchema(std::move(field.fields), resolved.fields);
    } else if (SameWireEncoding(field.type, resolved.type)) {
      field.type = resolved.type;
    }
  }
  return duckdb_schema;
}

}  // namespace

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
    if (DispositionName(disposition) == name) return disposition;
  }
  return std::nullopt;
}

std::optional<WriteDisposition> ParseWriteDisposition(std::string_view name) {
  for (const WriteDisposition disposition :
       {WriteDisposition::kWriteEmpty, WriteDisposition::kWriteAppend,
        WriteDisposition::kWriteTruncate, WriteDisposition::kWriteTruncateData}) {
    if (DispositionName(disposition) == name) return disposition;
  }
  return std::nullopt;
}

Emulator::Emulator(std::string data_dir, const std::vector<Project>& projects)
    : data_dir_(std::move(data_dir)) {
  const auto registry = std::filesystem::path(data_dir_) / "projects.json";
  std::map<std::string, Project> registered;
  if (!data_dir_.empty()) {
    std::filesystem::create_directories(data_dir_);
    if (std::filesystem::exists(registry)) {
      std::ifstream input(registry);
      if (!input) throw std::runtime_error("Cannot read " + registry.string());
      const json saved = json::parse(input);
      if (!saved.is_array()) throw std::invalid_argument("Project registry must be an array");
      for (const auto& value : saved) {
        Project project = ParseProject(value);
        if (!registered.emplace(project.project_id, project).second) {
          throw std::invalid_argument("Duplicate saved projectId: " + project.project_id);
        }
      }
    }
  }
  std::set<std::string> supplied;
  for (const Project& project : projects) {
    // Validate callers of the C++ boundary as well as CLI JSON.
    ParseProject(ProjectJson(project));
    if (!supplied.insert(project.project_id).second) {
      throw std::invalid_argument("Duplicate projectId: " + project.project_id);
    }
    registered.insert_or_assign(project.project_id, project);
  }
  for (const auto& [id, project] : registered) {
    project_ids_.emplace(id, id);
    projects_.push_back(project);
  }
  for (const Project& project : projects_) {
    if (!project.numeric_id) continue;
    const auto alias = project_ids_.emplace(*project.numeric_id, project.project_id).first;
    if (alias->second != project.project_id) {
      throw std::invalid_argument("Conflicting numericId: " + *project.numeric_id);
    }
  }
  for (const Project& project : projects_) {
    const std::string database = ProjectDatabase(project.project_id);
    try {
      backend_.Execute("ATTACH " + QuoteLiteral(database) + " AS " +
                       QuoteIdentifier(project.project_id));
      backend_.Execute("CREATE TABLE IF NOT EXISTS " + DatasetMetadataTable(project.project_id) +
                       " (dataset_id VARCHAR, metadata VARCHAR)");
    } catch (const BackendError& error) {
      throw ApiError::Internal("Failed to open " + database + ": " + error.what());
    }
  }
  if (!data_dir_.empty()) {
    json saved = json::array();
    for (const Project& project : projects_) saved.push_back(ProjectJson(project));
    const auto temporary = registry.string() + ".tmp";
    {
      std::ofstream output(temporary);
      output.exceptions(std::ios::failbit | std::ios::badbit);
      output << saved.dump(2) << '\n';
      output.close();
    }
    std::filesystem::rename(temporary, registry);
  }
}

const std::vector<Project>& Emulator::ListProjects() const { return projects_; }

std::string Emulator::ResolveProject(const std::string& project_id) const {
  const auto found = project_ids_.find(project_id);
  if (found == project_ids_.end()) throw ApiError::NotFound("Not found: Project " + project_id);
  return found->second;
}

std::string Emulator::ProjectDatabase(const std::string& project_id) const {
  if (data_dir_.empty()) return ":memory:";
  return (std::filesystem::path(data_dir_) / ProjectFileName(project_id)).string();
}

QueryResult Emulator::Execute(const std::string& sql, const std::vector<std::string>& setup,
                              bool null_arrays) {
  try {
    return backend_.Execute(sql, setup, null_arrays);
  } catch (const BackendError& error) {
    throw ApiError::InvalidQuery(error.what());
  }
}

QueryResult Emulator::Prepare(const std::string& sql, const std::vector<std::string>& setup) {
  try {
    return backend_.Prepare(sql, setup);
  } catch (const BackendError& error) {
    throw ApiError::InvalidQuery(error.what());
  }
}

TranslatedStatement Emulator::Translate(const std::string& query, const QueryParameters& parameters,
                                        const std::string& default_project,
                                        const std::string& default_dataset) {
  AnalyzerSettings settings{.default_project = default_project, .default_dataset = default_dataset};
  googlesql::TypeFactory type_factory;
  for (const FieldSchema& field : parameters.named_types()) {
    settings.named_parameters.emplace_back(field.name, ParameterType(field, type_factory));
  }
  for (const FieldSchema& field : parameters.positional_types()) {
    settings.positional_parameters.push_back(ParameterType(field, type_factory));
  }
  DuckDbTableSource source(backend_);
  BigQueryCatalog catalog(source, &type_factory, settings.default_project,
                          settings.default_dataset);
  const AnalyzerResult analyzed = AnalyzeGoogleSql(query, catalog, type_factory, settings);
  std::string unsupported;
  std::optional<TranslatedStatement> translated = TranslateStatement(
      analyzed.statement(), parameters,
      DefaultDataset{settings.default_project, settings.default_dataset}, &unsupported);
  if (!translated.has_value()) {
    throw ApiError::InvalidQuery("The emulator does not support " + unsupported);
  }
  return *std::move(translated);
}

std::shared_ptr<const Job> Emulator::RunJob(std::shared_ptr<Job> job,
                                            const std::function<void(Job&)>& body) {
  job->creation_time_ms = NowMillis();
  {
    std::scoped_lock lock(mutex_);
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
  job->end_time_ms = NowMillis();

  if (job->dry_run()) {
    return job;
  }
  std::scoped_lock lock(mutex_);
  const std::string key = JobKey(job->project_id, job->job_id);
  running_jobs_.erase(key);
  jobs_[key] = job;
  return job;
}

std::shared_ptr<const Job> Emulator::RunQuery(QueryRequest request) {
  request.project_id = ResolveProject(request.project_id);
  auto job = std::make_shared<Job>();
  job->project_id = request.project_id;
  job->job_id = request.job_id;
  job->configuration = QueryJob{.query = request.query,
                                .dry_run = request.dry_run,
                                .destination_table = request.destination_table,
                                .create_disposition = request.create_disposition,
                                .write_disposition = request.write_disposition};
  return RunJob(std::move(job), [&](Job& job) {
    if (request.destination_table) {
      auto& project = request.destination_table->project_id;
      project = ResolveProject(project.empty() ? request.project_id : project);
    }
    std::vector<std::string> setup;
    AnalyzerSettings settings{.default_project = request.project_id};
    if (request.default_dataset.has_value()) {
      const std::string dataset_project = ResolveProject(request.default_dataset->project_id.empty()
                                                             ? request.project_id
                                                             : request.default_dataset->project_id);
      setup.push_back("USE " + QualifiedName(DatasetReference{
                                   dataset_project, request.default_dataset->dataset_id}));
      settings.default_project = dataset_project;
      settings.default_dataset = request.default_dataset->dataset_id;
    } else {
      setup.push_back("USE " + QuoteIdentifier(request.project_id));
    }

    std::get<QueryJob>(job.configuration).destination_table = request.destination_table;

    if (const auto script = ParseScript(request.query)) {
      std::get<QueryJob>(job.configuration).statement_type = ScriptStatementType(*script);
      job.result =
          RunScript(request, *script, settings.default_project, settings.default_dataset, setup);
      return;
    }
    const TranslatedStatement translation = Translate(
        request.query, request.parameters, settings.default_project, settings.default_dataset);
    auto& query = std::get<QueryJob>(job.configuration);
    query.statement_type = translation.statement_type;
    query.ddl_target_table = translation.ddl_target_table;
    query.ddl_target_dataset = translation.ddl_target_dataset;
    if (!request.dry_run && !request.destination_table.has_value()) {
      job.result = RunStatement(translation, setup, request.null_arrays);
      return;
    }
    CheckDdlTarget(translation);
    if (request.destination_table.has_value() && !translation.result_schema.has_value()) {
      throw ApiError::Invalid("Cannot set destination table in jobs with DML/DDL statements");
    }
    QueryResult result;
    if (!request.dry_run) {
      result = WriteDestination(request.project_id, *request.destination_table,
                                request.create_disposition, request.write_disposition,
                                translation.sql, *translation.result_schema, setup);
    } else if (!AlterationStatements(translation).has_value()) {
      // A dry run checks an alteration against the table or dataset without running it.
      result = Prepare(translation.sql, setup);
    }
    if (translation.result_schema.has_value()) {
      result.schema = ReconcileSchema(std::move(result.schema), *translation.result_schema);
    }
    job.result = std::move(result);
  });
}

QueryResult Emulator::RunStatement(const TranslatedStatement& translation,
                                   const std::vector<std::string>& setup, bool null_arrays,
                                   Backend* session) {
  Backend& backend = session != nullptr ? *session : backend_;
  CheckDdlTarget(translation);
  QueryResult result;
  if (const std::optional<std::vector<std::string>> statements =
          AlterationStatements(translation)) {
    if (!statements->empty()) {
      backend.ExecuteDdl(statements->front(), {statements->begin() + 1, statements->end()}, "",
                         setup);
    }
  } else if (const std::optional<DdlWrite> write = MetadataWrite(translation)) {
    backend.ExecuteDdl(translation.sql, write->metadata_statements, write->skip_query, setup);
  } else {
    result = backend.Execute(translation.sql, setup, null_arrays);
  }
  if (translation.result_schema.has_value()) {
    result.schema = ReconcileSchema(std::move(result.schema), *translation.result_schema);
  }
  return result;
}

std::optional<std::vector<std::string>> Emulator::AlterationStatements(
    const TranslatedStatement& translation) {
  if (const auto& alteration = translation.altered_table) {
    const TableReference& table = alteration->table;
    if (ViewComment(backend_, table).has_value()) {
      throw ApiError::Invalid("ALTER TABLE cannot alter view " + TableName(table));
    }
    const std::optional<json> comment =
        RelationComment(backend_, "duckdb_tables()", "table_name", table);
    if (!comment.has_value()) {
      if (alteration->if_exists) {
        return std::vector<std::string>{};
      }
      throw ApiError::NotFound("Not found: Table " + TableName(table));
    }
    return AlterTableStatements(*alteration, TableSchema(backend_, table),
                                CommentMetadata(*comment));
  }
  if (const auto& alteration = translation.altered_dataset) {
    const DatasetReference& dataset = alteration->dataset;
    const QueryResult result = Execute(DatasetEntriesQuery(dataset.project_id, dataset.dataset_id));
    if (result.rows.empty()) {
      if (alteration->if_exists) {
        return std::vector<std::string>{};
      }
      throw ApiError::NotFound("Not found: Dataset " + dataset.project_id + ":" +
                               dataset.dataset_id);
    }
    return AlterDatasetStatements(*alteration, ParseDatasetMetadata(result.rows[0]["f"][1]["v"]));
  }
  return std::nullopt;
}

std::unique_ptr<TableSource> Emulator::NewTableSource(Backend* session) {
  return std::make_unique<DuckDbTableSource>(session != nullptr ? *session : backend_);
}

std::shared_ptr<const Job> Emulator::RunLoad(LoadRequest request) {
  request.project_id = ResolveProject(request.project_id);
  auto job = std::make_shared<Job>();
  job->project_id = request.project_id;
  job->job_id = request.job_id;
  job->configuration = request.load;
  return RunJob(std::move(job), [&](Job& job) {
    auto& load = std::get<LoadJob>(job.configuration);
    load.destination_table.project_id = ResolveProject(load.destination_table.project_id.empty()
                                                           ? request.project_id
                                                           : load.destination_table.project_id);
    load.configuration["destinationTable"]["projectId"] = load.destination_table.project_id;

    const json& config = load.configuration;
    const std::string format = config.value("sourceFormat", "CSV");
    if (format != "CSV" && format != "NEWLINE_DELIMITED_JSON" && format != "PARQUET") {
      throw ApiError::Invalid("Unsupported source format: " + format);
    }
    TemporaryFiles downloads;
    std::vector<std::string> paths = StageLoadSources(config, format, gcs_client_, downloads);
    std::vector<FieldSchema> requested_schema = SchemaFromJson(config.value("schema", json()));
    if (requested_schema.empty()) {
      TableReference destination = load.destination_table;
      if (destination.project_id.empty()) destination.project_id = job.project_id;
      try {
        requested_schema = GetTable(destination).schema;
      } catch (const ApiError& error) {
        if (error.http_status() != 404) throw;
      }
    }
    ParquetColumn parquet;
    if (format == "NEWLINE_DELIMITED_JSON") {
      paths = StageJsonNumerics(paths, requested_schema, downloads);
    } else if (format == "PARQUET") {
      ParquetSources sources = StageParquetDecimals(paths, downloads);
      paths = std::move(sources.paths);
      parquet = std::move(sources.columns);
      if (requested_schema.empty() && parquet.HasWideDecimal()) {
        requested_schema = Prepare(LoadQuery(format, paths, config, {})).schema;
        DetectParquetDecimals(requested_schema, parquet, config);
      }
    }
    const std::string sql = LoadQuery(format, paths, config, requested_schema, parquet);
    const QueryResult prepared = Prepare(sql);
    const QueryResult result = WriteDestination(
        job.project_id, load.destination_table, load.create_disposition, load.write_disposition,
        sql, requested_schema.empty() ? prepared.schema : requested_schema, {}, true);
    job.output_rows = std::stoll(result.rows.at(0).at("f").at(0).at("v").get<std::string>());
    job.result = QueryResult{};
  });
}

std::shared_ptr<const Job> Emulator::RunCopy(CopyRequest request) {
  request.project_id = ResolveProject(request.project_id);
  auto job = std::make_shared<Job>();
  job->project_id = request.project_id;
  job->job_id = request.job_id;
  job->configuration = request.copy;
  return RunJob(std::move(job), [&](Job& job) {
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
        copy.configuration["sourceTables"][i]["projectId"] = copy.source_tables[i].project_id;
      }
    }

    if (copy.source_tables.empty()) throw ApiError::Invalid("Source table is required");
    std::vector<FieldSchema> schema;
    std::string sql;
    for (TableReference source : copy.source_tables) {
      if (source.project_id.empty()) source.project_id = job.project_id;
      source.project_id = ResolveProject(source.project_id);
      const TableInfo table = GetTable(source);
      if (table.view_query) {
        throw ApiError::Invalid("Cannot copy a view: " + TableName(source));
      }
      if (sql.empty()) {
        schema = table.schema;
      } else if (SchemaToJson(table.schema) != SchemaToJson(schema)) {
        throw ApiError::Invalid("Source tables have different schemas");
      }
      sql += (sql.empty() ? "" : " UNION ALL ") + std::string("SELECT * FROM ") +
             QualifiedName(source);
    }
    const QueryResult result =
        WriteDestination(job.project_id, copy.destination_table, copy.create_disposition,
                         copy.write_disposition, sql, schema, {}, true);
    job.output_rows = std::stoll(result.rows.at(0).at("f").at(0).at("v").get<std::string>());
    job.result = QueryResult{};
  });
}

std::shared_ptr<const Job> Emulator::RunExtract(ExtractRequest request) {
  request.project_id = ResolveProject(request.project_id);
  auto job = std::make_shared<Job>();
  job->project_id = request.project_id;
  job->job_id = request.job_id;
  job->configuration = request.extract;
  return RunJob(std::move(job), [&](Job& job) {
    auto& extract = std::get<ExtractJob>(job.configuration);
    extract.source_table.project_id =
        ResolveProject(extract.source_table.project_id.empty() ? request.project_id
                                                               : extract.source_table.project_id);
    extract.configuration["sourceTable"]["projectId"] = extract.source_table.project_id;

    const json& config = extract.configuration;
    const std::string format = config.value("destinationFormat", "CSV");
    const std::string compression = config.value("compression", "NONE");
    if (format == "AVRO") throw ApiError::Invalid("The emulator does not support Avro extracts");
    if (format != "CSV" && format != "NEWLINE_DELIMITED_JSON" && format != "PARQUET") {
      throw ApiError::Invalid("Unsupported destination format: " + format);
    }
    const bool parquet = format == "PARQUET";
    if (compression != "NONE" && compression != "GZIP" &&
        !(parquet && (compression == "SNAPPY" || compression == "ZSTD"))) {
      throw ApiError::Invalid("Unsupported compression " + compression + " for " + format);
    }
    if (extract.destination_uris.size() != 1) {
      throw ApiError::Invalid("The emulator does not support multiple destination URIs");
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
    if (source.project_id.empty()) source.project_id = job.project_id;
    source.project_id = ResolveProject(source.project_id);
    const TableInfo table = GetTable(source);
    if (table.view_query) throw ApiError::Invalid("Cannot extract a view: " + TableName(source));
    if (parquet) {
      Execute(std::format("COPY (SELECT {} FROM {}) TO {} (FORMAT parquet, COMPRESSION {})",
                          ParquetExtractColumns(table.schema), QualifiedName(source),
                          QuoteLiteral(path),
                          compression == "NONE" ? "uncompressed" : ToLowerAscii(compression)));
      AnnotateParquetBigNumerics(path, table.schema);
    } else {
      WriteTextExtract(table.schema, Execute("SELECT * FROM " + QualifiedName(source)).rows,
                       {.json = format == "NEWLINE_DELIMITED_JSON",
                        .gzip = compression == "GZIP",
                        .field_delimiter = config.value("fieldDelimiter", ","),
                        .print_header = config.value("printHeader", true)},
                       path);
    }
    if (uri.starts_with("gs://")) gcs_client_.Upload(path, uri);
    job.result = QueryResult{};
  });
}

QueryResult Emulator::WriteDestination(const std::string& project_id, TableReference destination,
                                       CreateDisposition create, WriteDisposition write,
                                       const std::string& sql,
                                       const std::vector<FieldSchema>& schema,
                                       const std::vector<std::string>& setup, bool count_only) {
  std::set<std::string> names;
  std::string duplicates;
  for (const FieldSchema& field : schema) {
    std::string name = field.name;
    std::ranges::transform(name, name.begin(), [](unsigned char c) { return std::tolower(c); });
    if (!names.insert(name).second) {
      duplicates += (duplicates.empty() ? "" : ", ") + field.name;
    }
  }
  if (!duplicates.empty()) {
    throw ApiError::InvalidQuery(
        "Duplicate column names in the result are not supported. Found duplicate(s): " +
        duplicates);
  }

  if (destination.project_id.empty()) {
    destination.project_id = project_id;
  }
  destination.project_id = ResolveProject(destination.project_id);
  GetDataset(DatasetReference{destination.project_id, destination.dataset_id});
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

  // The result is materialized first, which runs the query once even when it reads the
  // destination itself. DuckDB lets a transaction write to a single database, so the temporary
  // table is filled before the transaction that writes the destination begins.
  const std::string result_table = "temp.main._bigquery_emulator_query_result";
  std::string aliases;
  for (const FieldSchema& field : schema) {
    aliases += (aliases.empty() ? "" : ", ") + QuoteIdentifier(field.name);
  }
  const std::string target = QualifiedName(destination);
  std::vector<std::string> statements = {
      std::format("CREATE TEMP TABLE _bigquery_emulator_query_result AS"
                  " SELECT * FROM ({}) AS _bigquery_emulator_query_result({})",
                  sql, aliases),
      "BEGIN TRANSACTION"};
  if (!existing.has_value() || write == WriteDisposition::kWriteTruncate) {
    if (existing.has_value()) {
      statements.push_back("DROP TABLE " + target);
    }
    // The columns take BigQuery's types for the result, so the table reads back as the query's
    // schema rather than as whatever DuckDB computed.
    if (const std::optional<std::string> columns = ColumnDefinitions(schema)) {
      statements.push_back(std::format("CREATE TABLE {} ({})", target, *columns));
      std::ranges::move(ColumnCommentStatements(destination, schema),
                        std::back_inserter(statements));
      std::ranges::move(RepeatedColumnDefaultStatements(destination, schema),
                        std::back_inserter(statements));
      statements.push_back("INSERT INTO " + target + " SELECT * FROM " + result_table);
    } else {
      statements.push_back("CREATE TABLE " + target + " AS SELECT * FROM " + result_table);
    }
  } else {
    if (write == WriteDisposition::kWriteTruncateData) {
      statements.push_back("DELETE FROM " + target);
    }
    statements.push_back("INSERT INTO " + target + " BY NAME SELECT * FROM " + result_table);
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

std::shared_ptr<const Job> Emulator::GetJob(std::string project_id, const std::string& job_id) {
  project_id = ResolveProject(project_id);
  std::scoped_lock lock(mutex_);
  const auto it = jobs_.find(JobKey(project_id, job_id));
  if (it == jobs_.end()) {
    throw ApiError::NotFound("Not found: Job " + project_id + ":" + job_id);
  }
  return it->second;
}

std::vector<std::shared_ptr<const Job>> Emulator::ListJobs(std::string project_id) {
  project_id = ResolveProject(project_id);
  std::scoped_lock lock(mutex_);
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

void Emulator::DeleteJob(std::string project_id, const std::string& job_id) {
  project_id = ResolveProject(project_id);
  std::scoped_lock lock(mutex_);
  if (jobs_.erase(JobKey(project_id, job_id)) == 0) {
    throw ApiError::NotFound("Not found: Job " + project_id + ":" + job_id);
  }
}

std::vector<std::string> Emulator::ListDatasets(std::string project_id) {
  project_id = ResolveProject(project_id);
  return FirstColumnStrings(Execute(DatasetsQuery(project_id)));
}

std::vector<DatasetListEntry> Emulator::ListDatasetEntries(std::string project_id) {
  project_id = ResolveProject(project_id);
  std::vector<DatasetListEntry> entries;
  for (const json& row : Execute(DatasetEntriesQuery(project_id)).rows) {
    // Keep the wire-field decoding beside the named result fields.
    // cppcheck-suppress useStlAlgorithm
    entries.push_back({.dataset_id = row["f"][0]["v"].get<std::string>(),
                       .metadata = ParseDatasetMetadata(row["f"][1]["v"])});
  }
  return entries;
}

DatasetMetadata Emulator::GetDataset(DatasetReference dataset) {
  dataset.project_id = ResolveProject(dataset.project_id);
  const QueryResult result = Execute(DatasetEntriesQuery(dataset.project_id, dataset.dataset_id));
  if (result.rows.empty()) {
    throw ApiError::NotFound("Not found: Dataset " + dataset.project_id + ":" + dataset.dataset_id);
  }
  return ParseDatasetMetadata(result.rows[0]["f"][1]["v"]);
}

void Emulator::CreateDataset(DatasetReference dataset, const DatasetMetadata& metadata) {
  dataset.project_id = ResolveProject(dataset.project_id);
  const std::vector<std::string> statements = DatasetMetadataStatements(dataset, metadata);
  try {
    backend_.ExecuteDdl("CREATE SCHEMA " + QualifiedName(dataset), statements, "");
  } catch (const BackendError& error) {
    if (std::string(error.what()).find("already exists") != std::string::npos) {
      throw ApiError::Duplicate("Already Exists: Dataset " + dataset.project_id + ":" +
                                dataset.dataset_id);
    }
    throw ApiError::Invalid(error.what());
  }
}

void Emulator::UpdateDataset(DatasetReference dataset, const DatasetMetadata& metadata) {
  dataset.project_id = ResolveProject(dataset.project_id);
  GetDataset(dataset);
  const std::vector<std::string> statements = DatasetMetadataStatements(dataset, metadata);
  try {
    backend_.ExecuteDdl(statements.front(), {statements.begin() + 1, statements.end()}, "");
  } catch (const BackendError& error) {
    throw ApiError::Invalid(error.what());
  }
}

void Emulator::DeleteDataset(DatasetReference dataset, bool delete_contents) {
  dataset.project_id = ResolveProject(dataset.project_id);
  GetDataset(dataset);
  if (!delete_contents && !ListTables(dataset).empty()) {
    throw ApiError::Invalid("Dataset " + dataset.project_id + ":" + dataset.dataset_id +
                            " is still in use");
  }
  try {
    backend_.ExecuteDdl(
        "DROP SCHEMA " + QualifiedName(dataset) + (delete_contents ? " CASCADE" : ""),
        DropDatasetWrite(dataset).metadata_statements, "");
  } catch (const BackendError& error) {
    throw ApiError::Invalid(error.what());
  }
}

std::vector<std::string> Emulator::ListTables(DatasetReference dataset) {
  dataset.project_id = ResolveProject(dataset.project_id);
  GetDataset(dataset);
  return FirstColumnStrings(Execute(TablesQuery(dataset)));
}

std::vector<TableListEntry> Emulator::ListTableEntries(DatasetReference dataset) {
  dataset.project_id = ResolveProject(dataset.project_id);
  GetDataset(dataset);
  const std::string where =
      std::format("WHERE database_name = {} AND schema_name = {}", QuoteLiteral(dataset.project_id),
                  QuoteLiteral(dataset.dataset_id));
  std::vector<TableListEntry> entries;
  for (const json& row :
       Execute(std::format("SELECT table_name, 'TABLE', comment FROM duckdb_tables() {}"
                           " UNION ALL SELECT view_name, 'VIEW', comment FROM duckdb_views() {}"
                           " ORDER BY 1",
                           where, where))
           .rows) {
    // Keep the wire-field decoding beside the named result fields.
    // cppcheck-suppress useStlAlgorithm
    entries.push_back({.table_id = row["f"][0]["v"].get<std::string>(),
                       .type = row["f"][1]["v"] == "VIEW" ? TableType::kView : TableType::kTable,
                       .metadata = CommentMetadata(row["f"][2]["v"])});
  }
  return entries;
}

TableInfo Emulator::GetTable(TableReference table, bool include_row_count) {
  table.project_id = ResolveProject(table.project_id);
  const DatasetReference dataset{table.project_id, table.dataset_id};
  GetDataset(dataset);
  TableInfo info;
  info.reference = table;
  if (const std::optional<json> comment = ViewComment(backend_, table); comment.has_value()) {
    std::optional<ViewMetadata> metadata = ParseViewMetadata(*comment);
    if (!metadata.has_value()) {
      throw ApiError::Invalid("View " + TableName(table) +
                              " was not created by the emulator and has no GoogleSQL definition");
    }
    info.view_query = std::move(metadata->query);
    info.schema = std::move(metadata->schema);
    info.metadata = std::move(metadata->metadata);
    return info;
  }
  try {
    info.schema = TableSchema(backend_, table);
    info.metadata = CommentMetadata(
        RelationComment(backend_, "duckdb_tables()", "table_name", table).value_or(nullptr));
    if (include_row_count) {
      const QueryResult count = backend_.Execute("SELECT count(*) FROM " + QualifiedName(table));
      info.num_rows = std::stoll(FirstColumnStrings(count).at(0));
    }
  } catch (const BackendError& error) {
    throw ApiError::NotFound("Not found: Table " + table.project_id + ":" + table.dataset_id + "." +
                             table.table_id);
  }
  return info;
}

void Emulator::CreateTable(TableReference table, const std::vector<FieldSchema>& schema,
                           const TableMetadata& metadata) {
  table.project_id = ResolveProject(table.project_id);
  GetDataset(DatasetReference{table.project_id, table.dataset_id});
  std::string columns;
  for (const FieldSchema& field : schema) {
    columns += (columns.empty() ? "" : ", ") + ColumnDefinition(field);
  }
  const DdlWrite write =
      CreateTableWrite(TableDefinition{.table = table, .schema = schema, .metadata = metadata});
  try {
    backend_.ExecuteDdl("CREATE TABLE " + QualifiedName(table) + " (" + columns + ")",
                        write.metadata_statements, write.skip_query);
  } catch (const BackendError& error) {
    if (std::string(error.what()).find("already exists") != std::string::npos) {
      throw ApiError::Duplicate("Already Exists: Table " + table.project_id + ":" +
                                table.dataset_id + "." + table.table_id);
    }
    throw ApiError::Invalid(error.what());
  }
}

void Emulator::CreateView(TableReference table, const json& definition,
                          const TableMetadata& metadata) {
  table.project_id = ResolveProject(table.project_id);
  WriteView(table, definition, metadata, /*replace=*/false);
}

void Emulator::UpdateTable(TableReference table,
                           const std::optional<std::vector<FieldSchema>>& schema,
                           const std::optional<json>& view,
                           const std::optional<TableMetadata>& metadata) {
  table.project_id = ResolveProject(table.project_id);
  const TableInfo info = GetTable(table, /*include_row_count=*/false);
  if (info.view_query.has_value()) {
    if (schema.has_value()) {
      throw ApiError::Invalid(
          "The emulator does not support changing the schema of a view; change its query instead");
    }
    if (view.has_value()) {
      // Every view the emulator keeps is GoogleSQL.
      json definition = {{"query", *info.view_query}, {"useLegacySql", false}};
      definition.update(*view);
      WriteView(table, definition, metadata.value_or(info.metadata), /*replace=*/true);
    } else if (metadata.has_value()) {
      Execute(ViewCommentStatement(table, {*info.view_query, info.schema, *metadata}));
    }
    return;
  }
  if (view.has_value()) {
    throw ApiError::Invalid("Table " + TableName(table) + " is not a view");
  }
  std::vector<std::string> statements;
  if (schema.has_value()) {
    statements = SchemaUpdateStatements(table, info.schema, *schema);
    std::ranges::move(ColumnCommentStatements(table, *schema), std::back_inserter(statements));
  }
  if (metadata.has_value()) {
    statements.push_back(TableCommentStatement(table, *metadata, schema.value_or(info.schema)));
  }
  if (statements.empty()) {
    return;
  }
  try {
    backend_.ExecuteDdl(statements.front(), {statements.begin() + 1, statements.end()}, "");
  } catch (const BackendError& error) {
    throw ApiError::Invalid(error.what());
  }
}

void Emulator::WriteView(TableReference table, const json& definition,
                         const TableMetadata& metadata, bool replace) {
  table.project_id = ResolveProject(table.project_id);
  GetDataset(DatasetReference{table.project_id, table.dataset_id});
  if (definition.value("useLegacySql", true)) {
    throw ApiError::Invalid("The emulator does not support legacy SQL views");
  }
  if (definition.contains("userDefinedFunctionResources")) {
    throw ApiError::Invalid("The emulator does not support view userDefinedFunctionResources");
  }
  const std::string query = definition.value("query", "");
  if (query.empty()) {
    throw ApiError::Invalid("View query is required");
  }
  try {
    const TranslatedStatement translation =
        Translate(std::string(replace ? "CREATE OR REPLACE VIEW " : "CREATE VIEW ") +
                      googlesql::ToIdentifierLiteral(table.project_id + "." + table.dataset_id +
                                                     "." + table.table_id) +
                      " AS " + query,
                  {}, table.project_id, "");
    if (!translation.view.has_value()) {
      throw ApiError::Internal("CREATE VIEW was not translated to a view");
    }
    ViewDefinition view = *translation.view;
    view.metadata = metadata;
    backend_.ExecuteDdl(translation.sql, CreateViewWrite(view).metadata_statements, "");
  } catch (const BackendError& error) {
    if (std::string(error.what()).find("already exists") != std::string::npos) {
      throw ApiError::Duplicate("Already Exists: Table " + TableName(table));
    }
    throw ApiError::Invalid(error.what());
  } catch (const std::runtime_error& error) {
    throw ApiError::Invalid(error.what());
  }
}

void Emulator::DeleteTable(TableReference table) {
  table.project_id = ResolveProject(table.project_id);
  // Views the emulator did not create have no metadata for GetTable, but can still be dropped.
  if (ViewComment(backend_, table).has_value()) {
    Execute("DROP VIEW " + QualifiedName(table));
    return;
  }
  GetTable(table, false);
  Execute("DROP TABLE " + QualifiedName(table));
}

QueryResult Emulator::ListTableData(TableReference table, int64_t start_index,
                                    int64_t max_results) {
  table.project_id = ResolveProject(table.project_id);
  if (GetTable(table).view_query) {
    throw ApiError::Invalid("Cannot read a view with tabledata.list; use a query instead");
  }
  return Execute("SELECT * FROM " + QualifiedName(table) + " LIMIT " + std::to_string(max_results) +
                 " OFFSET " + std::to_string(start_index));
}

std::vector<InsertError> Emulator::InsertTableData(TableReference table, const json& rows,
                                                   bool skip_invalid_rows,
                                                   bool ignore_unknown_values) {
  table.project_id = ResolveProject(table.project_id);
  if (!rows.is_array()) {
    throw ApiError::Invalid("rows must be an array");
  }
  const TableInfo info = GetTable(table, false);
  if (info.view_query) {
    throw ApiError::Invalid("Cannot insert into a view: " + TableName(table));
  }
  const std::vector<FieldSchema>& schema = info.schema;
  std::vector<InsertError> errors;
  std::vector<std::string> statements;
  std::vector<size_t> indexes;
  for (size_t i = 0; i < rows.size(); ++i) {
    try {
      if (!rows[i].is_object() || !rows[i].contains("json") || !rows[i]["json"].is_object()) {
        throw ApiError::Invalid("Row must contain a json object");
      }
      const json& values = rows[i]["json"];
      for (auto it = values.begin(); it != values.end(); ++it) {
        const bool known = std::ranges::any_of(
            schema, [&](const FieldSchema& field) { return field.name == it.key(); });
        if (!known && !ignore_unknown_values) {
          throw ApiError::Invalid("Unknown field: " + it.key());
        }
      }
      std::string columns;
      std::string literals;
      for (const FieldSchema& field : schema) {
        if (!columns.empty()) {
          columns += ", ";
          literals += ", ";
        }
        columns += QuoteIdentifier(field.name);
        const auto it = values.find(field.name);
        literals +=
            InsertValue(it == values.end() ? json(nullptr) : *it, field, ignore_unknown_values);
      }
      std::string statement = "INSERT INTO ";
      statement += QualifiedName(table);
      statement += " (";
      statement += columns;
      statement += ") VALUES (";
      statement += literals;
      statement += ")";
      statements.push_back(std::move(statement));
      indexes.push_back(i);
    } catch (const ApiError& error) {
      errors.push_back({i, error.what()});
    }
  }
  if (!skip_invalid_rows && !errors.empty()) {
    return errors;
  }
  try {
    for (const auto& [index, message] : backend_.InsertRows(statements, skip_invalid_rows)) {
      errors.push_back({indexes[index], message});
    }
  } catch (const BackendError& error) {
    throw ApiError::Invalid(error.what());
  }
  std::ranges::sort(errors,
                    [](const InsertError& a, const InsertError& b) { return a.index < b.index; });
  return errors;
}

}  // namespace bigquery_emulator_duckdb
