#include "src/emulator.h"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/statusor.h"
#include "googlesql/public/type.h"
#include "nlohmann/json.hpp"
#include "src/analyzer.h"
#include "src/api_error.h"
#include "src/backend.h"
#include "src/catalog.h"
#include "src/duckdb_sql.h"
#include "src/field_schema.h"
#include "src/frontend.h"
#include "src/resolved_translator.h"
#include "src/translator.h"

namespace bigquery_emulator_duckdb {
namespace {

using nlohmann::json;

int64_t NowMillis() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// Collapses whitespace runs so a multi-line statement logs as one line.
std::string OneLine(const std::string& sql) {
  std::string line;
  for (const char c : sql) {
    const bool space = c == ' ' || c == '\t' || c == '\n' || c == '\r';
    if (!space) {
      line += c;
    } else if (!line.empty() && line.back() != ' ') {
      line += ' ';
    }
  }
  if (!line.empty() && line.back() == ' ') {
    line.pop_back();
  }
  return line;
}

std::string QualifiedName(const DatasetReference& dataset) {
  return QuoteIdentifier(dataset.project_id) + "." + QuoteIdentifier(dataset.dataset_id);
}

std::string QualifiedName(const TableReference& table) {
  return QuoteIdentifier(table.project_id) + "." + QuoteIdentifier(table.dataset_id) + "." +
         QuoteIdentifier(table.table_id);
}

std::string JobKey(const std::string& project_id, const std::string& job_id) {
  return project_id + ":" + job_id;
}

std::vector<std::string> FirstColumnStrings(const QueryResult& result) {
  std::vector<std::string> values;
  values.reserve(result.rows.size());
  for (const json& row : result.rows) {
    values.push_back(row["f"][0]["v"].get<std::string>());
  }
  return values;
}

// Maps a BigQuery TableFieldSchema to a DuckDB column type.
std::string ToDuckDbType(const json& field) {
  const std::string type = field.value("type", "STRING");
  std::string duckdb_type;
  if (type == "INTEGER" || type == "INT64") {
    duckdb_type = "BIGINT";
  } else if (type == "FLOAT" || type == "FLOAT64") {
    duckdb_type = "DOUBLE";
  } else if (type == "BOOLEAN" || type == "BOOL") {
    duckdb_type = "BOOLEAN";
  } else if (type == "STRING" || type == "GEOGRAPHY") {
    duckdb_type = "VARCHAR";
  } else if (type == "BYTES") {
    duckdb_type = "BLOB";
  } else if (type == "DATE" || type == "TIME" || type == "JSON") {
    duckdb_type = type;
  } else if (type == "DATETIME") {
    duckdb_type = "TIMESTAMP";
  } else if (type == "TIMESTAMP") {
    duckdb_type = "TIMESTAMPTZ";
  } else if (type == "NUMERIC") {
    duckdb_type = "DECIMAL(38, 9)";
  } else if (type == "BIGNUMERIC") {
    duckdb_type = "DECIMAL(38, 19)";
  } else if (type == "RECORD" || type == "STRUCT") {
    duckdb_type = "STRUCT(";
    bool first = true;
    for (const json& child : field.value("fields", json::array())) {
      if (!first) {
        duckdb_type += ", ";
      }
      first = false;
      duckdb_type +=
          QuoteIdentifier(child.at("name").get<std::string>()) + " " + ToDuckDbType(child);
    }
    duckdb_type += ")";
  } else {
    throw ApiError::Invalid("Unsupported field type: " + type);
  }
  if (field.value("mode", "NULLABLE") == "REPEATED") {
    duckdb_type += "[]";
  }
  return duckdb_type;
}

// Serves the analyzer the tables the emulator keeps in DuckDB.
class DuckDbTableSource : public TableSource {
 public:
  explicit DuckDbTableSource(Backend& backend) : backend_(backend) {}

  std::optional<std::vector<FieldSchema>> FindTable(const std::string& project,
                                                    const std::string& dataset,
                                                    const std::string& table) override {
    try {
      return backend_
          .Prepare("SELECT * FROM " + QualifiedName(TableReference{project, dataset, table}))
          .schema;
    } catch (const BackendError&) {
      return std::nullopt;
    }
  }

 private:
  Backend& backend_;
};

std::vector<const googlesql::Type*> ParameterTypes(const std::vector<FieldSchema>& fields,
                                                   googlesql::TypeFactory& type_factory) {
  std::vector<const googlesql::Type*> types;
  for (const FieldSchema& field : fields) {
    absl::StatusOr<const googlesql::Type*> type = GoogleSqlType(field, &type_factory);
    if (!type.ok()) {
      throw ApiError::InvalidQuery(std::string(type.status().message()));
    }
    types.push_back(*type);
  }
  return types;
}

// Types whose values the backend writes the same way on the wire, so that a column DuckDB
// computed as one of them can be reported as another: all numbers are decimal strings, and
// these textual types are plain strings.
bool SameWireEncoding(const std::string& a, const std::string& b) {
  static const auto* const kGroups = new std::vector<std::set<std::string>>{
      {"INTEGER", "FLOAT", "NUMERIC", "BIGNUMERIC"}, {"STRING", "JSON", "GEOGRAPHY"}};
  if (a == b) {
    return true;
  }
  for (const std::set<std::string>& group : *kGroups) {
    if (group.contains(a) && group.contains(b)) {
      return true;
    }
  }
  return false;
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
    if (field.type == "RECORD" && resolved.type == "RECORD") {
      field.fields = ReconcileSchema(std::move(field.fields), resolved.fields);
    } else if (SameWireEncoding(field.type, resolved.type)) {
      field.type = resolved.type;
    }
  }
  return duckdb_schema;
}

}  // namespace

Emulator::Emulator(EmulatorOptions options) : options_(options) {}

void Emulator::EnsureProject(const std::string& project_id) {
  if (project_id.empty()) {
    throw ApiError::Invalid("Project id is required");
  }
  std::lock_guard<std::mutex> lock(mutex_);
  if (projects_.insert(project_id).second) {
    backend_.Execute("ATTACH ':memory:' AS " + QuoteIdentifier(project_id));
  }
}

QueryResult Emulator::Execute(const std::string& sql, const std::vector<std::string>& setup) {
  try {
    return backend_.Execute(sql, setup);
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

Emulator::Translation Emulator::Translate(const FrontendResult& frontend_result,
                                          const QueryParameters& parameters,
                                          AnalyzerSettings settings) {
  if (!IsQueryOrDml(frontend_result)) {
    return {TranslateToDuckDbSql(frontend_result, parameters), std::nullopt};
  }
  googlesql::TypeFactory type_factory;
  for (const FieldSchema& field : parameters.named_types()) {
    settings.named_parameters.emplace_back(field.name,
                                           ParameterTypes({field}, type_factory).front());
  }
  settings.positional_parameters = ParameterTypes(parameters.positional_types(), type_factory);
  DuckDbTableSource source(backend_);
  BigQueryCatalog catalog(source, &type_factory, settings.default_project,
                          settings.default_dataset);
  const AnalyzerResult analyzed =
      AnalyzeGoogleSql(frontend_result, catalog, type_factory, settings);
  std::optional<std::string> sql = TranslateResolvedToDuckDbSql(analyzed.statement(), parameters);
  if (!sql.has_value()) {
    switch (options_.parser_fallback) {
      case ParserFallback::kDeny:
        throw ApiError::InvalidQuery(
            "The resolved AST translator does not support this statement, and parser AST "
            "fallback is disabled");
      case ParserFallback::kWarn:
        std::cerr << "parser AST fallback: " << OneLine(frontend_result.sql()) << '\n';
        break;
      case ParserFallback::kAllow:
        break;
    }
    sql = TranslateToDuckDbSql(frontend_result, parameters);
  }
  return {*std::move(sql), analyzed.result_schema()};
}

std::shared_ptr<const Job> Emulator::RunQuery(const QueryRequest& request) {
  EnsureProject(request.project_id);
  auto job = std::make_shared<Job>();
  job->project_id = request.project_id;
  job->query = request.query;
  job->dry_run = request.dry_run;
  job->creation_time_ms = NowMillis();
  {
    std::lock_guard<std::mutex> lock(mutex_);
    job->job_id =
        request.job_id.empty() ? "job_" + std::to_string(next_job_number_++) : request.job_id;
  }

  std::vector<std::string> setup;
  AnalyzerSettings settings{.default_project = request.project_id};
  if (request.default_dataset.has_value()) {
    const std::string dataset_project = request.default_dataset->project_id.empty()
                                            ? request.project_id
                                            : request.default_dataset->project_id;
    EnsureProject(dataset_project);
    setup.push_back("USE " + QualifiedName(DatasetReference{dataset_project,
                                                            request.default_dataset->dataset_id}));
    settings.default_project = dataset_project;
    settings.default_dataset = request.default_dataset->dataset_id;
  } else {
    setup.push_back("USE " + QuoteIdentifier(request.project_id));
  }

  try {
    const FrontendResult frontend_result = ParseGoogleSql(request.query);
    const Translation translation = Translate(frontend_result, request.parameters, settings);
    QueryResult result =
        request.dry_run ? Prepare(translation.sql, setup) : Execute(translation.sql, setup);
    if (translation.schema.has_value()) {
      result.schema = ReconcileSchema(std::move(result.schema), *translation.schema);
    }
    job->result = std::move(result);
  } catch (const ApiError& error) {
    job->error = error;
  } catch (const std::exception& error) {
    job->error = ApiError::InvalidQuery(error.what());
  }
  job->end_time_ms = NowMillis();

  if (request.dry_run) {
    return job;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  jobs_[JobKey(request.project_id, job->job_id)] = job;
  return job;
}

std::shared_ptr<const Job> Emulator::GetJob(const std::string& project_id,
                                            const std::string& job_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto it = jobs_.find(JobKey(project_id, job_id));
  if (it == jobs_.end()) {
    throw ApiError::NotFound("Not found: Job " + project_id + ":" + job_id);
  }
  return it->second;
}

std::vector<std::string> Emulator::ListDatasets(const std::string& project_id) {
  EnsureProject(project_id);
  return FirstColumnStrings(
      Execute("SELECT schema_name FROM information_schema.schemata WHERE catalog_name = " +
              QuoteLiteral(project_id) +
              " AND schema_name NOT IN ('main', 'information_schema', 'pg_catalog')"
              " ORDER BY schema_name"));
}

void Emulator::GetDataset(const DatasetReference& dataset) {
  EnsureProject(dataset.project_id);
  const QueryResult result = Execute(
      "SELECT schema_name FROM information_schema.schemata WHERE catalog_name = " +
      QuoteLiteral(dataset.project_id) + " AND schema_name = " + QuoteLiteral(dataset.dataset_id));
  if (result.rows.empty()) {
    throw ApiError::NotFound("Not found: Dataset " + dataset.project_id + ":" + dataset.dataset_id);
  }
}

void Emulator::CreateDataset(const DatasetReference& dataset) {
  EnsureProject(dataset.project_id);
  try {
    backend_.Execute("CREATE SCHEMA " + QualifiedName(dataset));
  } catch (const BackendError& error) {
    throw ApiError::Duplicate("Already Exists: Dataset " + dataset.project_id + ":" +
                              dataset.dataset_id);
  }
}

void Emulator::DeleteDataset(const DatasetReference& dataset, bool delete_contents) {
  GetDataset(dataset);
  if (!delete_contents && !ListTables(dataset).empty()) {
    throw ApiError::Invalid("Dataset " + dataset.project_id + ":" + dataset.dataset_id +
                            " is still in use");
  }
  Execute("DROP SCHEMA " + QualifiedName(dataset) + (delete_contents ? " CASCADE" : ""));
}

std::vector<std::string> Emulator::ListTables(const DatasetReference& dataset) {
  GetDataset(dataset);
  return FirstColumnStrings(
      Execute("SELECT table_name FROM information_schema.tables WHERE table_catalog = " +
              QuoteLiteral(dataset.project_id) +
              " AND table_schema = " + QuoteLiteral(dataset.dataset_id) + " ORDER BY table_name"));
}

TableInfo Emulator::GetTable(const TableReference& table) {
  const DatasetReference dataset{table.project_id, table.dataset_id};
  GetDataset(dataset);
  TableInfo info;
  info.reference = table;
  try {
    info.schema = backend_.Execute("SELECT * FROM " + QualifiedName(table) + " LIMIT 0").schema;
    const QueryResult count = backend_.Execute("SELECT count(*) FROM " + QualifiedName(table));
    info.num_rows = std::stoll(FirstColumnStrings(count).at(0));
  } catch (const BackendError& error) {
    throw ApiError::NotFound("Not found: Table " + table.project_id + ":" + table.dataset_id + "." +
                             table.table_id);
  }
  return info;
}

void Emulator::CreateTable(const TableReference& table, const json& fields) {
  GetDataset(DatasetReference{table.project_id, table.dataset_id});
  std::string columns;
  for (const json& field : fields) {
    if (!columns.empty()) {
      columns += ", ";
    }
    columns += QuoteIdentifier(field.at("name").get<std::string>()) + " " + ToDuckDbType(field);
    if (field.value("mode", "NULLABLE") == "REQUIRED") {
      columns += " NOT NULL";
    }
  }
  try {
    backend_.Execute("CREATE TABLE " + QualifiedName(table) + " (" + columns + ")");
  } catch (const BackendError& error) {
    if (std::string(error.what()).find("already exists") != std::string::npos) {
      throw ApiError::Duplicate("Already Exists: Table " + table.project_id + ":" +
                                table.dataset_id + "." + table.table_id);
    }
    throw ApiError::Invalid(error.what());
  }
}

void Emulator::DeleteTable(const TableReference& table) {
  GetTable(table);
  Execute("DROP TABLE " + QualifiedName(table));
}

QueryResult Emulator::ListTableData(const TableReference& table, int64_t start_index,
                                    int64_t max_results) {
  GetTable(table);
  return Execute("SELECT * FROM " + QualifiedName(table) + " LIMIT " + std::to_string(max_results) +
                 " OFFSET " + std::to_string(start_index));
}

}  // namespace bigquery_emulator_duckdb
