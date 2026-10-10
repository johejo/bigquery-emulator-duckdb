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

namespace bigquery_emulator_duckdb {

// A materialized query result. Rows are already encoded in BigQuery's wire format
// ({"f": [{"v": ...}, ...]}) so that they can be returned to clients as is.
struct QueryResult {
  std::vector<FieldSchema> schema;
  std::vector<nlohmann::json> rows;
  bool has_rows = false;       // False for statements that produce no result set (DDL, SET, ...).
  int64_t affected_rows = -1;  // Rows changed by a DML statement, -1 when not applicable.

  [[nodiscard]] nlohmann::json SchemaToJson() const;
};

// Whether `schema` has a TIMESTAMP field anywhere, including inside a RECORD.
bool HasTimestampField(const std::vector<FieldSchema>& schema);

// Rewrites the TIMESTAMP cells of `row` from the epoch microseconds that results carry
// internally to BigQuery's default decimal-seconds spelling. Clients that ask for
// formatOptions.useInt64Timestamp are served the row as it is.
nlohmann::json TimestampsAsSeconds(const std::vector<FieldSchema>& schema,
                                   const nlohmann::json& row);

// Owns a DuckDB database instance. Calls use separate connections unless this is a session.
class Backend {
 public:
  explicit Backend(const std::optional<std::string>& session_user = std::nullopt);
  ~Backend();

  Backend(const Backend&) = delete;
  Backend& operator=(const Backend&) = delete;

  // A single-threaded connection to the same database. Its open transaction rolls back when
  // the session is destroyed; catalog reads and statements share its uncommitted changes.
  std::unique_ptr<Backend> NewSession();
  // Runs BEGIN TRANSACTION, COMMIT or ROLLBACK on a session.
  void Transaction(const std::string& statement);

  // Runs a single statement and materializes its result. `setup` statements run first on the
  // same connection and are used to select the default catalog and schema. A NULL array is
  // encoded as an empty one, as BigQuery returns it, unless `null_arrays` is set, which encodes
  // it as null for the emulator's own use of the value.
  QueryResult Execute(const std::string& sql, const std::vector<std::string>& setup = {},
                      bool null_arrays = false);

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

  // Inserts each row on one connection. Unless skip_invalid_rows is set, a failed row rolls
  // back the entire request. Returns the failed row indexes and their DuckDB errors.
  std::vector<std::pair<size_t, std::string>> InsertRows(const std::vector<std::string>& statements,
                                                         bool skip_invalid_rows);

 private:
  struct Database;
  struct Session;
  explicit Backend(std::shared_ptr<Database> database);
  std::shared_ptr<Database> db_;
  std::unique_ptr<Session> session_;
};

// Executes `sql` against a throwaway in-memory database and returns the first cell as text.
std::string ExecuteScalarString(const std::string& sql);

}  // namespace bigquery_emulator_duckdb
