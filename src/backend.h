#pragma once

#include <memory>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"
#include "src/field_schema.h"

namespace duckdb {
class DuckDB;
}  // namespace duckdb

namespace bigquery_emulator_duckdb {

// A materialized query result. Rows are already encoded in BigQuery's wire format
// ({"f": [{"v": ...}, ...]}) so that they can be returned to clients as is.
struct QueryResult {
  std::vector<FieldSchema> schema;
  std::vector<nlohmann::json> rows;
  bool has_rows = false;       // False for statements that produce no result set (DDL, SET, ...).
  int64_t affected_rows = -1;  // Rows changed by a DML statement, -1 when not applicable.

  nlohmann::json SchemaToJson() const;
};

// Whether `schema` has a TIMESTAMP field anywhere, including inside a RECORD.
bool HasTimestampField(const std::vector<FieldSchema>& schema);

// Rewrites the TIMESTAMP cells of `row` from the epoch microseconds that results carry
// internally to BigQuery's default decimal-seconds spelling. Clients that ask for
// formatOptions.useInt64Timestamp are served the row as it is.
nlohmann::json TimestampsAsSeconds(const std::vector<FieldSchema>& schema,
                                   const nlohmann::json& row);

class BackendError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

// Owns a DuckDB database instance. Thread-safe: every call uses its own connection.
class Backend {
 public:
  Backend();
  ~Backend();

  Backend(const Backend&) = delete;
  Backend& operator=(const Backend&) = delete;

  // Runs a single statement and materializes its result. `setup` statements run first on the
  // same connection and are used to select the default catalog and schema.
  QueryResult Execute(const std::string& sql, const std::vector<std::string>& setup = {});

  // Validates `sql` and returns the schema of its result without running it. The result never
  // has rows; `has_rows` says whether the statement produces a result set at all.
  QueryResult Prepare(const std::string& sql, const std::vector<std::string>& setup = {});

 private:
  std::unique_ptr<duckdb::DuckDB> db_;
};

// Executes `sql` against a throwaway in-memory database and returns the first cell as text.
std::string ExecuteScalarString(const std::string& sql);

}  // namespace bigquery_emulator_duckdb
