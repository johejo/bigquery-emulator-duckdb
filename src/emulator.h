#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

#include "nlohmann/json.hpp"
#include "src/api_error.h"
#include "src/backend.h"
#include "src/field_schema.h"
#include "src/gcs.h"
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
  std::optional<std::string> view_query;
};

struct InsertError {
  size_t index;
  std::string message;
};

// Where a job writes its destination table. The enumerators take BigQuery's names.
enum class CreateDisposition : std::uint8_t { kCreateIfNeeded, kCreateNever };
enum class WriteDisposition : std::uint8_t {
  kWriteEmpty,
  kWriteAppend,
  kWriteTruncate,
  kWriteTruncateData
};

// BigQuery's name for each disposition, and the disposition a name stands for.
std::string_view DispositionName(CreateDisposition disposition);
std::string_view DispositionName(WriteDisposition disposition);
std::optional<CreateDisposition> ParseCreateDisposition(std::string_view name);
std::optional<WriteDisposition> ParseWriteDisposition(std::string_view name);

struct QueryJob {
  std::string query;
  // A dry run job is validated but never executed, and it is not kept: BigQuery does not
  // create a job for it, so `GetJob` will not find it afterwards.
  bool dry_run = false;
  std::optional<TableReference> destination_table;
  CreateDisposition create_disposition = CreateDisposition::kCreateIfNeeded;
  WriteDisposition write_disposition = WriteDisposition::kWriteEmpty;
};

struct LoadJob {
  TableReference destination_table;
  CreateDisposition create_disposition = CreateDisposition::kCreateIfNeeded;
  WriteDisposition write_disposition = WriteDisposition::kWriteAppend;
  // The request's configuration.load, which also holds the source options.
  nlohmann::json configuration;
};

struct CopyJob {
  std::vector<TableReference> source_tables;
  TableReference destination_table;
  CreateDisposition create_disposition = CreateDisposition::kCreateIfNeeded;
  WriteDisposition write_disposition = WriteDisposition::kWriteEmpty;
  // The request's configuration.copy.
  nlohmann::json configuration;
};

struct Job {
  std::string project_id;
  std::string job_id;
  std::string location = "US";
  std::variant<QueryJob, LoadJob, CopyJob> configuration;
  // The rows a load or copy job wrote.
  int64_t output_rows = 0;
  int64_t creation_time_ms = 0;
  int64_t end_time_ms = 0;
  // Exactly one of `result` and `error` is set once the job is done. Jobs always complete
  // synchronously in the emulator.
  std::optional<QueryResult> result;
  std::optional<ApiError> error;

  // The query configuration, or null for other job types.
  const QueryJob* query() const { return std::get_if<QueryJob>(&configuration); }
  bool dry_run() const { return query() != nullptr && query()->dry_run; }
};

// A query to run as a job.
struct QueryRequest {
  std::string project_id;
  std::string query;
  std::optional<DatasetReference> default_dataset;
  std::string job_id;  // Generated when empty.
  QueryParameters parameters;
  bool dry_run = false;
  // Where a query's result is written.
  std::optional<TableReference> destination_table;
  CreateDisposition create_disposition = CreateDisposition::kCreateIfNeeded;
  WriteDisposition write_disposition = WriteDisposition::kWriteEmpty;
};

struct LoadRequest {
  std::string project_id;
  std::string job_id;  // Generated when empty.
  LoadJob load;
};

struct CopyRequest {
  std::string project_id;
  std::string job_id;  // Generated when empty.
  CopyJob copy;
};

// Emulator state: BigQuery projects map to DuckDB catalogs (attached databases), datasets to
// schemas and tables to tables. Jobs are kept in memory.
class Emulator {
 public:
  // With an empty `data_dir` every project is an in-memory database and nothing survives the
  // process. Otherwise each project is stored in its own DuckDB file under `data_dir`, which is
  // created when missing, and a project's data comes back the next time it is used.
  explicit Emulator(std::string data_dir = "");

  // The Storage API endpoint that load jobs read gs:// objects from.
  const std::string& storage_endpoint() const { return gcs_client_.endpoint(); }

  // Runs `request` as a job and returns it. A failed query is reported through the job's
  // error rather than thrown, which is how BigQuery reports it too.
  std::shared_ptr<const Job> RunQuery(const QueryRequest& request);
  std::shared_ptr<const Job> RunLoad(const LoadRequest& request);
  std::shared_ptr<const Job> RunCopy(const CopyRequest& request);
  std::shared_ptr<const Job> GetJob(const std::string& project_id, const std::string& job_id);
  std::vector<std::shared_ptr<const Job>> ListJobs(const std::string& project_id);
  void DeleteJob(const std::string& project_id, const std::string& job_id);

  std::vector<std::string> ListDatasets(const std::string& project_id);
  void GetDataset(const DatasetReference& dataset);  // Throws when the dataset is missing.
  void CreateDataset(const DatasetReference& dataset);
  void DeleteDataset(const DatasetReference& dataset, bool delete_contents);

  std::vector<std::string> ListTables(const DatasetReference& dataset);
  // The names of the views among ListTables, sorted.
  std::vector<std::string> ListViews(const DatasetReference& dataset);
  TableInfo GetTable(const TableReference& table, bool include_row_count = true);
  void CreateTable(const TableReference& table, const std::vector<FieldSchema>& schema);
  void CreateView(const TableReference& table, const nlohmann::json& definition);
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
    std::vector<std::string> view_metadata_statements;
    std::string view_existence_query;
  };
  // Keeps the catalog, types and resolved AST alive until translation finishes.
  Translation Translate(const std::string& query, const QueryParameters& parameters,
                        const std::string& default_project, const std::string& default_dataset);
  QueryResult Execute(const std::string& sql, const std::vector<std::string>& setup = {});
  QueryResult Prepare(const std::string& sql, const std::vector<std::string>& setup = {});
  // Registers `job` under its ID, generating one when it is empty, runs `body` on it and keeps
  // the finished job. A failure in `body` becomes the job's error. Dry runs are not registered.
  std::shared_ptr<const Job> RunJob(std::shared_ptr<Job> job,
                                    const std::function<void(Job&)>& body);
  // Runs the query `sql`, whose columns are `schema`, and writes its result to `destination`
  // with the given dispositions. Returns the query's result.
  QueryResult WriteDestination(const std::string& project_id, TableReference destination,
                               CreateDisposition create, WriteDisposition write,
                               const std::string& sql, const std::vector<FieldSchema>& schema,
                               const std::vector<std::string>& setup, bool count_only = false);

  Backend backend_;
  GcsClient gcs_client_;
  std::string data_dir_;
  std::mutex mutex_;
  std::unordered_set<std::string> projects_;
  std::unordered_map<std::string, std::shared_ptr<const Job>> jobs_;
  std::unordered_set<std::string> running_jobs_;
  int64_t next_job_number_ = 1;
};

}  // namespace bigquery_emulator_duckdb
