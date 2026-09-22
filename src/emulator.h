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
#include "src/api_error.h"
#include "src/backend.h"

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
  int64_t creation_time_ms = 0;
  int64_t end_time_ms = 0;
  // Exactly one of `result` and `error` is set once the job is done. Jobs always complete
  // synchronously in the emulator.
  std::optional<QueryResult> result;
  std::optional<ApiError> error;
};

// Emulator state: BigQuery projects map to DuckDB catalogs (attached databases), datasets to
// schemas and tables to tables. Jobs are kept in memory.
class Emulator {
 public:
  Emulator();

  // Runs `query` as a job and returns it. `job_id` is generated when empty.
  std::shared_ptr<const Job> RunQuery(const std::string& project_id, const std::string& query,
                                      const std::optional<DatasetReference>& default_dataset,
                                      const std::string& job_id);
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
  QueryResult Execute(const std::string& sql, const std::vector<std::string>& setup = {});

  Backend backend_;
  std::mutex mutex_;
  std::unordered_set<std::string> projects_;
  std::unordered_map<std::string, std::shared_ptr<const Job>> jobs_;
  int64_t next_job_number_ = 1;
};

}  // namespace bigquery_emulator_duckdb
