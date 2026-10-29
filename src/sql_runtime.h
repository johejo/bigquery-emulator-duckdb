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
#include <vector>

#include "nlohmann/json.hpp"
#include "src/backend.h"
#include "src/backend_initializer.h"
#include "src/builtin_function_options.h"
#include "src/external_table_provider.h"
#include "src/field_schema.h"
#include "src/function_options.h"
#include "src/googlesql_functions_duckdb/objectref_options.h"
#include "src/googlesql_functions_duckdb/options.h"
#include "src/googlesql_geography/options.h"
#include "src/location.h"
#include "src/project.h"
#include "src/query_parameters.h"
#include "src/references.h"
#include "src/routine.h"
#include "src/storage_snapshot.h"
#include "src/system_variables.h"
#include "src/table_metadata.h"
#include "src/table_source.h"
#include "src/translated_statement.h"

// Declared rather than included to keep the generated GoogleSQL protocol buffer headers it needs
// out of this header, which most of the application includes.
namespace bigquery_emulator_duckdb::function_catalog {
class Catalog;
}

namespace bigquery_emulator_duckdb {

// JobStatistics2.statementType of a multi-statement query.
inline constexpr char kScriptStatementType[] = "SCRIPT";

// What kind of table a dataset holds.
enum class TableType : std::uint8_t { kTable, kSnapshot, kView, kMaterializedView, kExternal };

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
  bool materialized = false;
  std::optional<MaterializedViewSource> materialized_source = {};
};

// Rejects a query, load or Storage write to a snapshot or external table.
void RejectReadOnlyWrite(const TableInfo& table);

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

// Statement configuration and output metadata shared with the application's query jobs.
// There is no job ID, registry, timing or error bookkeeping here.
struct QueryJob {
  std::string query;
  // A dry run is validated but never executed. The application does not register its job.
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

// SQL execution options shared by runtime callers and the application's query requests.
struct QueryRequest {
  std::string project_id;
  // Where the job runs, as the request names it; see JobLocation.
  std::optional<std::string> location;
  std::string query;
  std::optional<DatasetReference> default_dataset;
  std::string job_id;  // Application envelope; ignored by SqlRuntime.
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

// Shared SQL execution and native catalog state. Application composition supplies optional
// service capabilities; an unconfigured runtime never constructs external-service clients.
class SqlRuntime {
 public:
  // Restores persisted registrations and replaces those explicitly supplied at startup.
  // With no data directory, registrations and data live in memory only. A nonempty
  // session_user enables SESSION_USER for every client. Service capabilities are supplied
  // by the application and are not persisted. `initialize` registers application-specific
  // backend functions, such as the JavaScript and Python UDF adapters.
  // Defaults select BigQuery function corrections, built-in exclusions and outward geography
  // bounds. An empty `excluded_function_ids` exposes GoogleSQL-only built-ins too.
  // `declarations` independently selects whether to add BigQuery-specific declarations.
  // The provider factory runs only for statements, after construction. Callbacks and providers
  // must retain any state their deferred reads need through result/snapshot consumption.
  explicit SqlRuntime(
      std::string data_dir = "", const std::vector<Project>& projects = {},
      const std::optional<std::string>& session_user = std::nullopt,
      const BackendInitializer& initialize = {},
      const functions_duckdb::ObjectStorage& storage = functions_duckdb::ObjectStorage(),
      const DatabaseInitializer& initialize_database = {},
      std::function<std::unique_ptr<ExternalTableProvider>(Backend&)> external_tables = {},
      const googlesql_geography::Options& geography = {.expand_bounding_box = true},
      const functions_duckdb::Options& functions = BigQueryFunctionOptions(),
      ExcludedFunctionIds excluded_function_ids = BigQueryExcludedFunctionIds(),
      FunctionDeclarations declarations = FunctionDeclarations::kBigQuery);
  virtual ~SqlRuntime() = default;

  const std::vector<Project>& ListProjects() const;
  std::string ResolveProject(const std::string& project_id) const;

  // Executes without registering a REST job. The caller initializes `location` from the
  // request's location; the runtime updates it and the statement metadata as execution proceeds,
  // including before a failure. The application decides how to record exceptions.
  QueryResult ExecuteQuery(QueryRequest request, QueryJob& query, JobLocation& location);
  // Runs `sql`, DuckDB statements rather than a query job, on the emulator's database, where
  // each project is a catalog, and returns the last statement's result. For debugging.
  QueryResult ExecuteDuckDb(const std::string& sql);
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
                       const TableMetadata& metadata, bool materialized = false);
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
  // The application uses native writes and location checks for load/copy/extract; friendship
  // keeps those orchestration-only operations out of the public client-facing catalog API.
  friend class Emulator;

  // Runs multi-statement queries on the same runtime and session as single statements.
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
      bool stores_result = false, bool dry_run = false);
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
  // TABLE, and rejects DROP or replacement of a materialized view with a different object type.
  static void CheckRelationTarget(Backend& backend, const TranslatedStatement& translation);
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
                 const TableMetadata& metadata, bool replace, bool materialized = false);
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
  // The translator's functions, chosen by the same options as the backend's.
  const function_catalog::Catalog& functions_;
  ExcludedFunctionIds excluded_function_ids_;
  FunctionDeclarations declarations_;
  bool has_session_user_;
  std::function<std::unique_ptr<ExternalTableProvider>(Backend&)> external_tables_;
  std::string data_dir_;
  // DuckPGQ updates the graph registry of every connection when a graph changes.
  std::mutex graph_mutex_;
  // Keeps each table's rows and the write that changes them together, by the table's DuckDB name;
  // see HistoryMutex.
  std::mutex history_mutexes_mutex_;
  std::unordered_map<std::string, std::unique_ptr<std::mutex>> history_mutexes_;
  std::vector<Project> projects_;
  std::unordered_map<std::string, std::string> project_ids_;
  // Names the database of each multi-statement query's temporary tables.
  std::atomic<int64_t> next_script_number_ = 1;
};

}  // namespace bigquery_emulator_duckdb
