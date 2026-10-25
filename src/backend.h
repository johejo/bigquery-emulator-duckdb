#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "nlohmann/json.hpp"
#include "src/field_schema.h"
#include "src/function_options.h"
#include "src/googlesql_functions_duckdb/objectref_options.h"
#include "src/googlesql_functions_duckdb/options.h"
#include "src/googlesql_geography/options.h"
#include "src/result_rows.h"
#include "src/storage_snapshot.h"
#include "src/udf/javascript/limits.h"
#include "src/udf/python/options.h"

namespace bigquery_emulator_duckdb {

// A query result, whose rows are encoded in BigQuery's wire format as they are read.
struct QueryResult {
  std::vector<FieldSchema> schema;
  ResultRows rows;
  bool has_rows = false;       // False for statements that produce no result set (DDL, SET, ...).
  int64_t affected_rows = -1;  // Rows changed by a DML statement, -1 when not applicable.

  [[nodiscard]] nlohmann::json SchemaToJson() const;
};

// Owns a DuckDB database instance. Calls use separate connections unless this is a session.
// Defaults select BigQuery function corrections and outward geography bounds.
class Backend {
 public:
  explicit Backend(
      const std::optional<std::string>& session_user = std::nullopt,
      const udf::JavaScriptLimits& javascript = udf::JavaScriptLimits(),
      const functions_duckdb::ObjectStorage& storage = functions_duckdb::ObjectStorage(),
      const udf::PythonOptions& python = udf::PythonOptions(),
      const googlesql_geography::Options& geography = {.expand_bounding_box = true},
      const functions_duckdb::Options& functions = BigQueryFunctionOptions());
  ~Backend();

  Backend(const Backend&) = delete;
  Backend& operator=(const Backend&) = delete;

  // A single-threaded connection to the same database. Its open transaction rolls back when
  // the session is destroyed; catalog reads and statements share its uncommitted changes.
  std::unique_ptr<Backend> NewSession();
  // Runs BEGIN TRANSACTION, COMMIT or ROLLBACK on a session.
  void Transaction(const std::string& statement);
  // Whether this is a session with an open transaction.
  [[nodiscard]] bool InTransaction() const;
  // Runs `statement` when the session's open transaction commits, before COMMIT, once however
  // often it is given.
  void BeforeCommit(const std::string& statement);

  // Runs a single statement and materializes its result. `setup` statements run first on the
  // same connection and are used to select the default catalog and schema. A NULL array is
  // encoded as an empty one, as BigQuery returns it, unless `null_arrays` is set, which encodes
  // it as null for the emulator's own use of the value.
  QueryResult Execute(const std::string& sql, const std::vector<std::string>& setup = {},
                      bool null_arrays = false);

  // Runs `sql` on a session of its own and keeps its result, whose columns are the fields of
  // `schema`, as a Storage Read snapshot. The session lives as long as the snapshot.
  std::shared_ptr<const StorageSnapshot> ExecuteStorageRead(const std::string& sql,
                                                            std::vector<FieldSchema> schema);

  // Runs `statements` in order on one connection and materializes the result of the last one.
  // Statements before the last may open a transaction. Outside a session, a failure rolls it
  // back; in a session, it stays open until ROLLBACK or the session is destroyed.
  QueryResult ExecuteAll(const std::vector<std::string>& statements,
                         const std::vector<std::string>& setup = {}, bool null_arrays = false);

  // Runs the DDL statement `sql` and then the statements that record its metadata, in one
  // transaction, so that neither is kept without the other. When skip_query is not empty and
  // returns a row, nothing runs: that is how IF NOT EXISTS and IF EXISTS are honored when the
  // metadata statements would otherwise fail or overwrite what is there.
  void ExecuteDdl(const std::string& sql, const std::vector<std::string>& metadata_statements,
                  const std::string& skip_query, const std::vector<std::string>& setup = {});

  // Validates `sql` and returns the schema of its result without running it. The result never
  // has rows; `has_rows` says whether the statement produces a result set at all.
  QueryResult Prepare(const std::string& sql, const std::vector<std::string>& setup = {});

  // Why the columns of `relation`, a table or view, cannot be read, naming the first column of a
  // DuckDB type the emulator does not support, or nullopt.
  std::optional<std::string> UnreadableColumnError(const std::string& relation);

  // A SELECT that converts external numeric columns to the emulator's query representation,
  // or nullopt when the stored values already have that representation. Such external tables
  // can be read but cannot yet be written through the emulator.
  std::optional<std::string> NumericReadQuery(const std::string& relation);

  // Inserts each row on one connection. Unless skip_invalid_rows is set, a failed row rolls
  // back the entire request. Returns the failed row indexes and their DuckDB errors.
  std::vector<std::pair<size_t, std::string>> InsertRows(const std::vector<std::string>& statements,
                                                         bool skip_invalid_rows);

 private:
  struct Database;
  struct Session;
  class CallConnection;
  explicit Backend(std::shared_ptr<Database> database);
  // The session's connection, or outside a session a new one that closes with the result.
  [[nodiscard]] CallConnection Connect() const;
  std::shared_ptr<Database> db_;
  std::unique_ptr<Session> session_;
};

}  // namespace bigquery_emulator_duckdb
