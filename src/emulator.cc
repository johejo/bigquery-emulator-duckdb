#include "src/emulator.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "nlohmann/json.hpp"
#include "src/api_error.h"
#include "src/backend.h"
#include "src/frontend.h"
#include "src/translator.h"

namespace bigquery_emulator_duckdb {
namespace {

using nlohmann::json;

int64_t NowMillis() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::string QuoteIdentifier(const std::string& identifier) {
  std::string quoted = "\"";
  for (const char c : identifier) {
    quoted += c;
    if (c == '"') {
      quoted += '"';
    }
  }
  return quoted + "\"";
}

std::string QuoteLiteral(const std::string& text) {
  std::string quoted = "'";
  for (const char c : text) {
    quoted += c;
    if (c == '\'') {
      quoted += '\'';
    }
  }
  return quoted + "'";
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

}  // namespace

Emulator::Emulator() = default;

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

std::shared_ptr<const Job> Emulator::RunQuery(
    const std::string& project_id, const std::string& query,
    const std::optional<DatasetReference>& default_dataset, const std::string& job_id) {
  EnsureProject(project_id);
  auto job = std::make_shared<Job>();
  job->project_id = project_id;
  job->query = query;
  job->creation_time_ms = NowMillis();
  {
    std::lock_guard<std::mutex> lock(mutex_);
    job->job_id = job_id.empty() ? "job_" + std::to_string(next_job_number_++) : job_id;
  }

  std::vector<std::string> setup;
  if (default_dataset.has_value()) {
    const std::string dataset_project =
        default_dataset->project_id.empty() ? project_id : default_dataset->project_id;
    EnsureProject(dataset_project);
    setup.push_back("USE " +
                    QualifiedName(DatasetReference{dataset_project, default_dataset->dataset_id}));
  } else {
    setup.push_back("USE " + QuoteIdentifier(project_id));
  }

  try {
    const FrontendResult frontend_result = ParseGoogleSql(query);
    const std::string duckdb_sql = TranslateToDuckDbSql(frontend_result);
    job->result = Execute(duckdb_sql, setup);
  } catch (const ApiError& error) {
    job->error = error;
  } catch (const std::exception& error) {
    job->error = ApiError::InvalidQuery(error.what());
  }
  job->end_time_ms = NowMillis();

  std::lock_guard<std::mutex> lock(mutex_);
  jobs_[JobKey(project_id, job->job_id)] = job;
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
