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
#include "src/backend_functions/javascript_limits.h"
#include "src/external_query.h"
#include "src/field_schema.h"
#include "src/gcs.h"
#include "src/location.h"
#include "src/project.h"
#include "src/query_parameters.h"
#include "src/references.h"
#include "src/routine.h"
#include "src/storage_snapshot.h"
#include "src/system_variables.h"
#include "src/table_metadata.h"

namespace bigquery_emulator_duckdb {

class TableSource;
struct TranslatedStatement;

// JobStatistics2.statementType of a multi-statement query.
inline constexpr char kScriptStatementType[] = "SCRIPT";

// What kind of table a dataset holds. The emulator creates only tables, table snapshots and views.
enum class TableType : std::uint8_t { kTable, kSnapshot, kView };

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
  std::string location = kDefaultLocation;  // The location of the table's dataset.
  int64_t num_rows = 0;
  int64_t storage_id = 0;  // DuckDB table OID; distinguishes a dropped/recreated destination.
  std::optional<std::string> view_query;
};

// Throws ApiError::Invalid with BigQuery's message for a query, load or Storage write when `table`
// is a table snapshot, whose rows cannot change.
void RejectSnapshotWrite(const TableInfo& table);

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
  kWriteTruncateData,
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
  std::optional<RoutineReference> ddl_target_routine = {};
};

struct LoadJob {
  TableReference destination_table;
  CreateDisposition create_disposition = CreateDisposition::kCreateIfNeeded;
  WriteDisposition write_disposition = WriteDisposition::kWriteAppend;
  // The request's configuration.load, which also holds the source options.
  nlohmann::json configuration;
};

// What a copy job makes of its source: a table of the same type, a table snapshot of a table, a
// standard table of a snapshot, or a clone. The enumerators take BigQuery's names.
enum class CopyOperation : std::uint8_t { kCopy, kSnapshot, kRestore, kClone };

struct CopyJob {
  CopyOperation operation = CopyOperation::kCopy;
  std::vector<TableReference> source_tables;
  TableReference destination_table;
  CreateDisposition create_disposition = CreateDisposition::kCreateIfNeeded;
  WriteDisposition write_disposition = WriteDisposition::kWriteEmpty;
  // destinationExpirationTime, in milliseconds since the Unix epoch.
  std::optional<int64_t> destination_expiration_time;
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
  std::string location = kDefaultLocation;
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
  [[nodiscard]] const QueryJob* query() const { return std::get_if<QueryJob>(&configuration); }
  [[nodiscard]] bool dry_run() const { return query() != nullptr && query()->dry_run; }
};

// A query to run as a job.
struct QueryRequest {
  std::string project_id;
  // Where the job runs, as the request names it; see JobLocation.
  std::optional<std::string> location;
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
  std::optional<std::string> location;
  std::string job_id;  // Generated when empty.
  LoadJob load;
};

struct CopyRequest {
  std::string project_id;
  std::optional<std::string> location;
  std::string job_id;  // Generated when empty.
  CopyJob copy;
};

struct ExtractRequest {
  std::string project_id;
  std::optional<std::string> location;
  std::string job_id;  // Generated when empty.
  ExtractJob extract;
};

// Emulator state: BigQuery projects map to DuckDB catalogs (attached databases), datasets to
// schemas and tables to tables. Jobs are kept in memory.
class Emulator {
 public:
  // Restores persisted registrations and replaces those explicitly supplied at startup.
  // With no data directory, registrations and data live in memory only. A nonempty
  // session_user enables SESSION_USER for every client; it and `connections`, which
  // EXTERNAL_QUERY reaches, are not persisted. JavaScript UDFs run under `javascript`.
  explicit Emulator(
      std::string data_dir = "", const std::vector<Project>& projects = {},
      const std::optional<std::string>& session_user = std::nullopt,
      const backend_functions::JavaScriptLimits& javascript = backend_functions::JavaScriptLimits(),
      const std::vector<ExternalConnection>& connections = {});
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
  // A job in `location`, as a request names it. A request that names none finds a job in the US
  // or EU multi-region, as BigQuery requires the location of any other.
  std::shared_ptr<const Job> GetJob(std::string project_id, const std::string& job_id,
                                    const std::optional<std::string>& location = std::nullopt);
  std::vector<std::shared_ptr<const Job>> ListJobs(std::string project_id);
  void DeleteJob(std::string project_id, const std::string& job_id,
                 const std::optional<std::string>& location = std::nullopt);

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
  TableInfo GetTable(TableReference table, bool include_row_count = true, bool expire = true);
  // Return the mutation result before another request can expire the relation.
  TableInfo CreateTable(TableReference table, const std::vector<FieldSchema>& schema,
                        const TableMetadata& metadata = {});
  TableInfo CreateView(TableReference table, const nlohmann::json& definition,
                       const TableMetadata& metadata = {});
  // tables.patch and tables.update: gives a table the schema `schema`, or a view the definition
  // `view`, and either the metadata `metadata`. What is left out is kept, including the fields
  // of `view` it omits.
  TableInfo UpdateTable(TableReference table, const std::optional<std::vector<FieldSchema>>& schema,
                        const std::optional<nlohmann::json>& view,
                        const std::optional<TableMetadata>& metadata = std::nullopt,
                        bool rounding_mode_unspecified = false);
  void DeleteTable(TableReference table);
  // The persistent UDFs of `dataset`, sorted by id.
  std::vector<Routine> ListRoutines(DatasetReference dataset);
  Routine GetRoutine(RoutineReference routine);
  Routine WriteRoutine(Routine routine, bool update);
  void DeleteRoutine(RoutineReference routine);
  // Reads the rows of `table` from `start_index` on, with only the fields `selected_fields` names,
  // as SelectFields takes them, unless it is empty.
  QueryResult ListTableData(TableReference table, int64_t start_index, int64_t max_results,
                            std::string_view selected_fields);
  // Materializes a Storage Read snapshot of the selected fields, or all of them, without
  // creating a query job. The restriction is parsed as one GoogleSQL expression before it is
  // included in the SELECT.
  std::shared_ptr<const StorageSnapshot> ReadStorageTable(
      TableReference table, const std::vector<std::string>& selected_fields,
      const std::string& restriction);
  // Validates and inserts the whole Storage Write batch in one transaction. With validate_only,
  // rolls it back, for pending/buffered appends. Checks the schema in that same transaction.
  std::vector<InsertError> WriteStorageRows(TableReference table,
                                            const std::vector<FieldSchema>& schema,
                                            const nlohmann::json& rows, int64_t storage_id,
                                            bool validate_only = false);
  std::vector<InsertError> InsertTableData(TableReference table, const nlohmann::json& rows,
                                           bool skip_invalid_rows, bool ignore_unknown_values);

 private:
  // Runs the statements of the multi-statement queries of RunQuery; in emulator_query.cc.
  class ScriptHostImpl;

  // The registered project's DuckDB file path, or ":memory:".
  std::string ProjectDatabase(const std::string& project_id) const;
  // Keeps the catalog, types and resolved AST alive until translation finishes. What the query
  // uses counts toward `location` when it is set. The query can read `system_variables`.
  TranslatedStatement Translate(
      const std::string& query, const QueryParameters& parameters,
      const std::string& default_project, const std::string& default_dataset,
      JobLocation* location = nullptr,
      const googlesql::SystemVariableValuesMap* system_variables = nullptr,
      bool stores_result = false);
  // The location of `dataset`, as `backend` sees it, or nothing when it does not exist.
  static std::optional<std::string> DatasetLocation(Backend& backend,
                                                    const DatasetReference& dataset);
  // Takes for `location` the location of the datasets and INFORMATION_SCHEMA regions that the
  // table names of the script `query` name, as BigQuery infers a job's location before running
  // it, or the US when they name several. Names of no dataset that exists are left out.
  void InferLocation(const std::string& query, const std::string& default_project,
                     const std::string& default_dataset, JobLocation& location);
  // Records that a job in `location` uses the dataset of `table`, if it exists.
  void UseDataset(JobLocation& location, const TableReference& table);
  // Records the datasets that the DDL statement `translation` targets as used by the job in
  // `location`. The location a CREATE SCHEMA names must be the job's.
  static void UseTargets(Backend& backend, const TranslatedStatement& translation,
                         JobLocation& location);
  // What gives the columns of `table`, created with `schema` rather than by CREATE TABLE, the
  // defaults of their defaultValueExpression, translated as CREATE TABLE ... DEFAULT translates
  // them. Throws ApiError::Invalid for an expression BigQuery would reject or the emulator cannot
  // translate.
  std::vector<std::string> ColumnDefaultStatements(const TableReference& table,
                                                   const std::vector<FieldSchema>& schema);
  QueryResult Execute(const std::string& sql, const std::vector<std::string>& setup = {},
                      bool null_arrays = false);
  // Runs `translation` as a query job without a destination table does, recording the metadata
  // of a DDL statement, and returns its result with the schema the translation gives it.
  // A dataset that CREATE SCHEMA creates without a location is created in the job's.
  QueryResult RunStatement(const TranslatedStatement& translation,
                           const std::vector<std::string>& setup, bool null_arrays = false,
                           Backend* session = nullptr, JobLocation* location = nullptr);
  // Rejects DDL targets in DuckDB's internal schemas.
  static void CheckDdlTarget(const TranslatedStatement& translation);
  // Rejects DDL that would change a table snapshot, as `backend` sees it, other than DROP SNAPSHOT
  // TABLE, and DROP SNAPSHOT TABLE of a table that is not one.
  static void CheckSnapshotTarget(Backend& backend, const TranslatedStatement& translation);
  // The DuckDB statements that apply the ALTER TABLE or ALTER SCHEMA `translation` to what the
  // table or dataset has now, which are none when IF EXISTS finds nothing to alter, or nothing
  // for any other statement.
  std::optional<std::vector<std::string>> AlterationStatements(
      const TranslatedStatement& translation);
  // The metadata of the dataset the table or view that `translation` creates belongs to, as
  // `backend` sees it, or none for any other statement, a temporary table or a missing dataset.
  static DatasetMetadata CreatedInDataset(Backend& backend, const TranslatedStatement& translation);
  // A catalog source for the tables, views and datasets the emulator holds; what it finds counts
  // toward `location` when it is set.
  std::unique_ptr<TableSource> NewTableSource(Backend* session = nullptr, bool expire = true,
                                              JobLocation* location = nullptr);
  QueryResult Prepare(const std::string& sql, const std::vector<std::string>& setup = {});
  // Creates the view `table` from the ViewDefinition `definition` with `metadata`, replacing the
  // one there when `replace` is set; a view created anew takes its dataset's default expiration.
  void WriteView(TableReference table, const nlohmann::json& definition,
                 const TableMetadata& metadata, bool replace);
  // Registers `job` under its ID, generating one when it is empty, runs `body` on it and keeps
  // the finished job, which runs where `location` settles. A failure in `body` becomes the job's
  // error. Dry runs are not registered.
  std::shared_ptr<const Job> RunJob(std::shared_ptr<Job> job, JobLocation& location,
                                    const std::function<void(Job&)>& body);
  // Keeps the rows `table`, as `backend` sees it, has before a write to it on `backend`, for FOR
  // SYSTEM_TIME AS OF. Outside a transaction, the returned lock keeps other writes from reaching
  // the table until the caller's write is done; in one, the rows are kept until it commits.
  [[nodiscard]] std::unique_lock<std::mutex> KeepVersion(Backend& backend,
                                                         const TableReference& table);
  // The mutex that writes to `table` outside a transaction hold while they keep its rows and change
  // them, so that each kept version is the rows the previous write committed. Writes to other
  // tables do not wait for it. Drops forgotten history of `table`'s project first.
  std::mutex& HistoryMutex(const TableReference& table);
  // The creation time of `table`, a table the emulator keeps the history of, or 0 for one it does
  // not, such as a temporary table or one created before tables recorded their creation time.
  [[nodiscard]] int64_t HistoryCreationTime(const TableReference& table,
                                            const TableMetadata& metadata) const;
  // Runs the query `sql`, whose columns are `schema`, and writes its result to `destination`
  // with the given dispositions. A table the write creates takes `metadata`; a write to an
  // existing table rejects metadata. A `copy` job's new table keeps the rounding modes of
  // `schema` and takes no default rounding mode, and a `restore` keeps the metadata of the table
  // it replaces. Returns the query's result.
  QueryResult WriteDestination(const std::string& project_id, TableReference destination,
                               CreateDisposition create, WriteDisposition write,
                               const std::string& sql, const std::vector<FieldSchema>& schema,
                               const std::vector<std::string>& setup, bool count_only = false,
                               const TableMetadata& metadata = TableMetadata(), bool copy = false,
                               bool restore = false);

  // Mutations also hold this while reading their response. Recursive because their catalog
  // lookups may expire other relations on the same thread.
  std::recursive_mutex expiration_mutex_;
  Backend backend_;
  bool has_session_user_;
  ExternalConnections connections_{.backend = backend_};
  GcsClient gcs_client_;
  std::string data_dir_;
  std::mutex mutex_;
  // Keeps each table's rows and the write that changes them together, by the table's DuckDB name;
  // see HistoryMutex.
  std::mutex history_mutexes_mutex_;
  std::unordered_map<std::string, std::unique_ptr<std::mutex>> history_mutexes_;
  std::vector<Project> projects_;
  std::unordered_map<std::string, std::string> project_ids_;
  std::unordered_map<std::string, std::shared_ptr<const Job>> jobs_;
  std::unordered_set<std::string> running_jobs_;
  int64_t next_job_number_ = 1;
  // Names the database of each multi-statement query's temporary tables.
  std::atomic<int64_t> next_script_number_ = 1;
};

}  // namespace bigquery_emulator_duckdb
