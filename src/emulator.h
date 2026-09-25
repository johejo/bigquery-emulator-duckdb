#pragma once

#include <cstddef>
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

struct InsertError {
  size_t index;
  std::string message;
};

struct Job {
  std::string project_id;
  std::string job_id;
  std::string location = "US";
  std::string query;
  bool is_load = false;
  nlohmann::json load_configuration;
  int64_t output_rows = 0;
  // A dry run job is validated but never executed, and it is not kept: BigQuery does not
  // create a job for it, so `GetJob` will not find it afterwards.
  bool dry_run = false;
  std::optional<TableReference> destination_table;
  std::string create_disposition;
  std::string write_disposition;
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
  // Where a query's result is written. The dispositions take BigQuery's names; empty means the
  // default, CREATE_IF_NEEDED and WRITE_EMPTY.
  std::optional<TableReference> destination_table;
  std::string create_disposition;
  std::string write_disposition;
};

struct LoadRequest {
  std::string project_id;
  std::string job_id;
  TableReference destination_table;
  nlohmann::json configuration;
};

// Emulator state: BigQuery projects map to DuckDB catalogs (attached databases), datasets to
// schemas and tables to tables. Jobs are kept in memory.
class Emulator {
 public:
  // With an empty `data_dir` every project is an in-memory database and nothing survives the
  // process. Otherwise each project is stored in its own DuckDB file under `data_dir`, which is
  // created when missing, and a project's data comes back the next time it is used.
  explicit Emulator(std::string data_dir = "");

  // Runs `request` as a job and returns it. A failed query is reported through the job's
  // error rather than thrown, which is how BigQuery reports it too.
  std::shared_ptr<const Job> RunQuery(const QueryRequest& request);
  std::shared_ptr<const Job> RunLoad(const LoadRequest& request);
  std::shared_ptr<const Job> GetJob(const std::string& project_id, const std::string& job_id);
  std::vector<std::shared_ptr<const Job>> ListJobs(const std::string& project_id);
  void DeleteJob(const std::string& project_id, const std::string& job_id);

  std::vector<std::string> ListDatasets(const std::string& project_id);
  void GetDataset(const DatasetReference& dataset);  // Throws when the dataset is missing.
  void CreateDataset(const DatasetReference& dataset);
  void DeleteDataset(const DatasetReference& dataset, bool delete_contents);

  std::vector<std::string> ListTables(const DatasetReference& dataset);
  TableInfo GetTable(const TableReference& table);
  void CreateTable(const TableReference& table, const nlohmann::json& fields);
  void DeleteTable(const TableReference& table);
  QueryResult ListTableData(const TableReference& table, int64_t start_index, int64_t max_results);
  std::vector<InsertError> InsertTableData(const TableReference& table, const nlohmann::json& rows,
                                           bool skip_invalid_rows, bool ignore_unknown_values);

 private:
  void EnsureProject(const std::string& project_id);
  // What EnsureProject attaches for `project_id`: a DuckDB file path, or ":memory:".
  std::string ProjectDatabase(const std::string& project_id) const;
  struct Translation {
    std::string sql;
    std::optional<std::vector<FieldSchema>> schema;
  };
  // Keeps the catalog, types and resolved AST alive until translation finishes.
  Translation Translate(const std::string& query, const QueryParameters& parameters,
                        const std::string& default_project, const std::string& default_dataset);
  QueryResult Execute(const std::string& sql, const std::vector<std::string>& setup = {});
  QueryResult Prepare(const std::string& sql, const std::vector<std::string>& setup = {});
  // Runs the query `sql`, whose columns are `schema`, and writes its result to `destination`, the
  // request's destination table. Returns the query's result.
  QueryResult WriteDestination(const QueryRequest& request, TableReference destination,
                               const std::string& sql, const std::vector<FieldSchema>& schema,
                               const std::vector<std::string>& setup, bool count_only = false);

  Backend backend_;
  std::string data_dir_;
  std::mutex mutex_;
  std::unordered_set<std::string> projects_;
  std::unordered_map<std::string, std::shared_ptr<const Job>> jobs_;
  std::unordered_set<std::string> running_jobs_;
  int64_t next_job_number_ = 1;
};

}  // namespace bigquery_emulator_duckdb
