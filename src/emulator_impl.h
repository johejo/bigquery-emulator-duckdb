#pragma once

// The state and the helpers of Emulator, which only the emulator's own sources include.

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "nlohmann/json.hpp"
#include "src/backend.h"
#include "src/emulator.h"
#include "src/field_schema.h"
#include "src/gcs.h"
#include "src/project.h"
#include "src/query_parameters.h"
#include "src/references.h"
#include "src/routine.h"
#include "src/table_metadata.h"

namespace bigquery_emulator_duckdb {

class TableSource;
struct TranslatedStatement;

// What Emulator does; each public member implements the Emulator member of the same name.
class Emulator::Impl {
 public:
  Impl(std::string data_dir, const std::vector<Project>& projects,
       const std::optional<std::string>& session_user);
  const std::vector<Project>& ListProjects() const;
  std::string ResolveProject(const std::string& project_id) const;
  const std::string& storage_endpoint() const { return gcs_client_.endpoint(); }

  std::shared_ptr<const Job> RunQuery(QueryRequest request);
  std::shared_ptr<const Job> RunLoad(LoadRequest request);
  std::shared_ptr<const Job> RunCopy(CopyRequest request);
  std::shared_ptr<const Job> RunExtract(ExtractRequest request);
  std::shared_ptr<const Job> GetJob(std::string project_id, const std::string& job_id);
  std::vector<std::shared_ptr<const Job>> ListJobs(std::string project_id);
  void DeleteJob(std::string project_id, const std::string& job_id);

  std::vector<std::string> ListDatasets(std::string project_id);
  std::vector<DatasetListEntry> ListDatasetEntries(std::string project_id);
  DatasetMetadata GetDataset(DatasetReference dataset);
  void CreateDataset(DatasetReference dataset, const DatasetMetadata& metadata = {});
  void UpdateDataset(DatasetReference dataset, const DatasetMetadata& metadata);
  void DeleteDataset(DatasetReference dataset, bool delete_contents);

  std::vector<std::string> ListTables(DatasetReference dataset);
  std::vector<TableListEntry> ListTableEntries(DatasetReference dataset);
  TableInfo GetTable(TableReference table, bool include_row_count = true, bool expire = true);
  TableInfo CreateTable(TableReference table, const std::vector<FieldSchema>& schema,
                        const TableMetadata& metadata = {});
  TableInfo CreateView(TableReference table, const nlohmann::json& definition,
                       const TableMetadata& metadata = {});
  TableInfo UpdateTable(TableReference table, const std::optional<std::vector<FieldSchema>>& schema,
                        const std::optional<nlohmann::json>& view,
                        const std::optional<TableMetadata>& metadata = std::nullopt);
  void DeleteTable(TableReference table);
  std::vector<Routine> ListRoutines(DatasetReference dataset);
  Routine GetRoutine(RoutineReference routine);
  Routine WriteRoutine(Routine routine, bool update);
  void DeleteRoutine(RoutineReference routine);
  QueryResult ListTableData(TableReference table, int64_t start_index, int64_t max_results);
  QueryResult ReadStorageTable(TableReference table, const std::string& restriction);
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
  // Keeps the catalog, types and resolved AST alive until translation finishes.
  TranslatedStatement Translate(const std::string& query, const QueryParameters& parameters,
                                const std::string& default_project,
                                const std::string& default_dataset);
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
  QueryResult RunStatement(const TranslatedStatement& translation,
                           const std::vector<std::string>& setup, bool null_arrays = false,
                           Backend* session = nullptr);
  // Rejects DDL targets in DuckDB's internal schemas.
  static void CheckDdlTarget(const TranslatedStatement& translation);
  // The DuckDB statements that apply the ALTER TABLE or ALTER SCHEMA `translation` to what the
  // table or dataset has now, which are none when IF EXISTS finds nothing to alter, or nothing
  // for any other statement.
  std::optional<std::vector<std::string>> AlterationStatements(
      const TranslatedStatement& translation);
  // The metadata of the dataset the table or view that `translation` creates belongs to, as
  // `backend` sees it, or none for any other statement, a temporary table or a missing dataset.
  static DatasetMetadata CreatedInDataset(Backend& backend, const TranslatedStatement& translation);
  // A catalog source for the tables, views and datasets the emulator holds.
  std::unique_ptr<TableSource> NewTableSource(Backend* session = nullptr, bool expire = true);
  QueryResult Prepare(const std::string& sql, const std::vector<std::string>& setup = {});
  // Creates the view `table` from the ViewDefinition `definition` with `metadata`, replacing the
  // one there when `replace` is set; a view created anew takes its dataset's default expiration.
  void WriteView(TableReference table, const nlohmann::json& definition,
                 const TableMetadata& metadata, bool replace);
  // Registers `job` under its ID, generating one when it is empty, runs `body` on it and keeps
  // the finished job. A failure in `body` becomes the job's error. Dry runs are not registered.
  std::shared_ptr<const Job> RunJob(std::shared_ptr<Job> job,
                                    const std::function<void(Job&)>& body);
  // Runs the query `sql`, whose columns are `schema`, and writes its result to `destination`
  // with the given dispositions. A table the write creates takes `metadata`; a write to an
  // existing table rejects metadata. Returns the query's result.
  QueryResult WriteDestination(const std::string& project_id, TableReference destination,
                               CreateDisposition create, WriteDisposition write,
                               const std::string& sql, const std::vector<FieldSchema>& schema,
                               const std::vector<std::string>& setup, bool count_only = false,
                               const TableMetadata& metadata = {});

  // Mutations also hold this while reading their response. Recursive because their catalog
  // lookups may expire other relations on the same thread.
  std::recursive_mutex expiration_mutex_;
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
