#pragma once

#include <atomic>
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
#include "src/catalog.h"
#include "src/field_schema.h"
#include "src/gcs.h"
#include "src/project.h"
#include "src/query_parameters.h"
#include "src/references.h"
#include "src/table_metadata.h"
#include "src/translator.h"

namespace googlesql {
class ParserOutput;
}

namespace bigquery_emulator_duckdb {

// JobStatistics2.statementType of a multi-statement query.
inline constexpr char kScriptStatementType[] = "SCRIPT";

// The location of every dataset, table and job. The emulator does not model locations.
inline constexpr char kLocation[] = "US";

// What kind of table a dataset holds. The emulator creates only tables and views.
enum class TableType : std::uint8_t { kTable, kView };

struct TableListEntry {
  std::string table_id;
  TableType type = TableType::kTable;
  TableMetadata metadata;
};

struct DatasetListEntry {
  std::string dataset_id;
  DatasetMetadata metadata;
};

struct TableInfo {
  TableReference reference;
  std::vector<FieldSchema> schema;
  TableMetadata metadata;
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
  // What the query's statement is, once it has been translated: JobStatistics2.statementType and
  // the target of a DDL statement.
  std::string statement_type = {};
  std::optional<TableReference> ddl_target_table = {};
  std::optional<DatasetReference> ddl_target_dataset = {};
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

struct ExtractJob {
  TableReference source_table;
  std::vector<std::string> destination_uris;
  // The request's configuration.extract, which also holds the format options.
  nlohmann::json configuration;
};

struct Job {
  std::string project_id;
  std::string job_id;
  std::string location = kLocation;
  std::variant<QueryJob, LoadJob, CopyJob, ExtractJob> configuration;
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
  // Keeps a NULL array in the result of a single statement null rather than empty, as BigQuery
  // returns it. No API sets it: GoogleSQL's compliance tests compare results with it set.
  bool null_arrays = false;
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

struct ExtractRequest {
  std::string project_id;
  std::string job_id;  // Generated when empty.
  ExtractJob extract;
};

// Emulator state: BigQuery projects map to DuckDB catalogs (attached databases), datasets to
// schemas and tables to tables. Jobs are kept in memory.
class Emulator {
 public:
  // Restores persisted registrations and replaces those explicitly supplied at startup.
  // With no data directory, registrations and data live in memory only. A nonempty
  // session_user enables SESSION_USER for every client; it is not persisted.
  explicit Emulator(std::string data_dir = "", const std::vector<Project>& projects = {},
                    const std::optional<std::string>& session_user = std::nullopt);
  const std::vector<Project>& ListProjects() const;
  std::string ResolveProject(const std::string& project_id) const;

  // The Storage API endpoint that load jobs read gs:// objects from and extract jobs write to.
  const std::string& storage_endpoint() const { return gcs_client_.endpoint(); }

  // Runs `request` as a job and returns it. A failed query is reported through the job's
  // error rather than thrown, which is how BigQuery reports it too.
  std::shared_ptr<const Job> RunQuery(QueryRequest request);
  std::shared_ptr<const Job> RunLoad(LoadRequest request);
  std::shared_ptr<const Job> RunCopy(CopyRequest request);
  std::shared_ptr<const Job> RunExtract(ExtractRequest request);
  std::shared_ptr<const Job> GetJob(std::string project_id, const std::string& job_id);
  std::vector<std::shared_ptr<const Job>> ListJobs(std::string project_id);
  void DeleteJob(std::string project_id, const std::string& job_id);

  std::vector<std::string> ListDatasets(std::string project_id);
  // The datasets of ListDatasets, each with its metadata.
  std::vector<DatasetListEntry> ListDatasetEntries(std::string project_id);
  // The metadata of `dataset`. Throws when the dataset is missing.
  DatasetMetadata GetDataset(DatasetReference dataset);
  void CreateDataset(DatasetReference dataset, const DatasetMetadata& metadata = {});
  // datasets.patch and datasets.update: gives `dataset` the metadata `metadata`.
  void UpdateDataset(DatasetReference dataset, const DatasetMetadata& metadata);
  void DeleteDataset(DatasetReference dataset, bool delete_contents);

  std::vector<std::string> ListTables(DatasetReference dataset);
  // The tables of ListTables, each with its type.
  std::vector<TableListEntry> ListTableEntries(DatasetReference dataset);
  TableInfo GetTable(TableReference table, bool include_row_count = true);
  void CreateTable(TableReference table, const std::vector<FieldSchema>& schema,
                   const TableMetadata& metadata = {});
  void CreateView(TableReference table, const nlohmann::json& definition,
                  const TableMetadata& metadata = {});
  // tables.patch and tables.update: gives a table the schema `schema`, or a view the definition
  // `view`, and either the metadata `metadata`. What is left out is kept, including the fields
  // of `view` it omits.
  void UpdateTable(TableReference table, const std::optional<std::vector<FieldSchema>>& schema,
                   const std::optional<nlohmann::json>& view,
                   const std::optional<TableMetadata>& metadata = std::nullopt);
  void DeleteTable(TableReference table);
  QueryResult ListTableData(TableReference table, int64_t start_index, int64_t max_results);
  std::vector<InsertError> InsertTableData(TableReference table, const nlohmann::json& rows,
                                           bool skip_invalid_rows, bool ignore_unknown_values);

 private:
  // Evaluates the statements and expressions of a multi-statement query; in emulator_script.cc.
  friend class ScriptEvaluator;

  // The registered project's DuckDB file path, or ":memory:".
  std::string ProjectDatabase(const std::string& project_id) const;
  // Keeps the catalog, types and resolved AST alive until translation finishes.
  TranslatedStatement Translate(const std::string& query, const QueryParameters& parameters,
                                const std::string& default_project,
                                const std::string& default_dataset);
  QueryResult Execute(const std::string& sql, const std::vector<std::string>& setup = {},
                      bool null_arrays = false);
  // Runs `translation` as a query job without a destination table does, recording the metadata
  // of a DDL statement, and returns its result with the schema the translation gives it.
  QueryResult RunStatement(const TranslatedStatement& translation,
                           const std::vector<std::string>& setup, bool null_arrays = false,
                           Backend* session = nullptr);
  // The DuckDB statements that apply the ALTER TABLE or ALTER SCHEMA `translation` to what the
  // table or dataset has now, which are none when IF EXISTS finds nothing to alter, or nothing
  // for any other statement.
  std::optional<std::vector<std::string>> AlterationStatements(
      const TranslatedStatement& translation);
  // A catalog source for the tables, views and datasets the emulator holds.
  std::unique_ptr<TableSource> NewTableSource(Backend* session = nullptr);
  // The script that `query` is, or null when it is a single statement or does not parse, which
  // RunQuery then reports as it does for a single statement.
  static std::unique_ptr<googlesql::ParserOutput> ParseScript(const std::string& query);

  // TEMP declarations followed by one SELECT are a SELECT job; other statement lists are SCRIPT.
  static std::string ScriptStatementType(const googlesql::ParserOutput& script);
  // Runs the multi-statement query `script`, the text of `request.query`, with `setup` selecting
  // the default dataset `default_project`.`default_dataset`, and returns the result of the last
  // statement it ran. Rejects a request a multi-statement query cannot be.
  QueryResult RunScript(const QueryRequest& request, const googlesql::ParserOutput& script,
                        const std::string& default_project, const std::string& default_dataset,
                        const std::vector<std::string>& setup);
  QueryResult Prepare(const std::string& sql, const std::vector<std::string>& setup = {});
  // Creates the view `table` from the ViewDefinition `definition` with `metadata`, replacing the
  // one there when `replace` is set.
  void WriteView(TableReference table, const nlohmann::json& definition,
                 const TableMetadata& metadata, bool replace);
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
  bool has_session_user_;
  GcsClient gcs_client_;
  std::string data_dir_;
  std::mutex mutex_;
  std::vector<Project> projects_;
  std::unordered_map<std::string, std::string> project_ids_;
  std::unordered_map<std::string, std::shared_ptr<const Job>> jobs_;
  std::unordered_set<std::string> running_jobs_;
  int64_t next_job_number_ = 1;
  // Names the database of each multi-statement query's temporary tables.
  std::atomic<int64_t> next_script_number_ = 1;
};

}  // namespace bigquery_emulator_duckdb
