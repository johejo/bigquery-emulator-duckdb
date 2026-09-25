#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "nlohmann/json.hpp"
#include "src/analyzer.h"
#include "src/api_error.h"
#include "src/backend.h"
#include "src/field_schema.h"
#include "src/query_parameters.h"

namespace bigquery_emulator_duckdb {

struct DatasetReference {
  std::string project_id;
  std::string dataset_id;
};

struct TableReference {
  std::string project_id;
  std::string dataset_id;
  std::string table_id;
};

struct TableInfo {
  TableReference reference;
  std::vector<FieldSchema> schema;
  int64_t num_rows = 0;
};

struct Job {
  std::string project_id;
  std::string job_id;
  std::string location = "US";
  std::string query;
  // A dry run job is validated but never executed, and it is not kept: BigQuery does not
  // create a job for it, so `GetJob` will not find it afterwards.
  bool dry_run = false;
  int64_t creation_time_ms = 0;
  int64_t end_time_ms = 0;
  // Exactly one of `result` and `error` is set once the job is done. Jobs always complete
  // synchronously in the emulator.
  std::optional<QueryResult> result;
  std::optional<ApiError> error;
};

// A query to run as a job.
struct QueryRequest {
  std::string project_id;
  std::string query;
  std::optional<DatasetReference> default_dataset;
  std::string job_id;  // Generated when empty.
  QueryParameters parameters;
  bool dry_run = false;
};

// Emulator state: BigQuery projects map to DuckDB catalogs (attached databases), datasets to
// schemas and tables to tables. Jobs are kept in memory.
class Emulator {
 public:
  Emulator();

  // Runs `request` as a job and returns it. A failed query is reported through the job's
  // error rather than thrown, which is how BigQuery reports it too.
  std::shared_ptr<const Job> RunQuery(const QueryRequest& request);
  std::shared_ptr<const Job> GetJob(const std::string& project_id, const std::string& job_id);

  std::vector<std::string> ListDatasets(const std::string& project_id);
  void GetDataset(const DatasetReference& dataset);  // Throws when the dataset is missing.
  void CreateDataset(const DatasetReference& dataset);
  void DeleteDataset(const DatasetReference& dataset, bool delete_contents);

  std::vector<std::string> ListTables(const DatasetReference& dataset);
  TableInfo GetTable(const TableReference& table);
  void CreateTable(const TableReference& table, const nlohmann::json& fields);
  void DeleteTable(const TableReference& table);
  QueryResult ListTableData(const TableReference& table, int64_t start_index, int64_t max_results);

 private:
  void EnsureProject(const std::string& project_id);
  struct Translation {
    std::string sql;
    std::optional<std::vector<FieldSchema>> schema;
  };
  // Keeps the catalog, types and resolved AST alive until translation finishes.
  Translation Translate(const std::string& query, const QueryParameters& parameters,
                        AnalyzerSettings settings);
  QueryResult Execute(const std::string& sql, const std::vector<std::string>& setup = {});
  QueryResult Prepare(const std::string& sql, const std::vector<std::string>& setup = {});

  Backend backend_;
  std::mutex mutex_;
  std::unordered_set<std::string> projects_;
  std::unordered_map<std::string, std::shared_ptr<const Job>> jobs_;
  int64_t next_job_number_ = 1;
};

}  // namespace bigquery_emulator_duckdb
