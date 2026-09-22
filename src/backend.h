#pragma once

#include <memory>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"

namespace duckdb {
class DuckDB;
}  // namespace duckdb

namespace bigquery_emulator_duckdb {

// A column of a query result described with BigQuery's TableFieldSchema vocabulary.
struct FieldSchema {
  std::string name;
  std::string type;                 // INTEGER, FLOAT, STRING, BOOLEAN, TIMESTAMP, RECORD, ...
  std::string mode;                 // NULLABLE or REPEATED
  std::vector<FieldSchema> fields;  // Populated for RECORD.

  nlohmann::json ToJson() const;
};

// A materialized query result. Rows are already encoded in BigQuery's wire format
// ({"f": [{"v": ...}, ...]}) so that they can be returned to clients as is.
struct QueryResult {
  std::vector<FieldSchema> schema;
  std::vector<nlohmann::json> rows;
  bool has_rows = false;       // False for statements that produce no result set (DDL, SET, ...).
  int64_t affected_rows = -1;  // Rows changed by a DML statement, -1 when not applicable.

  nlohmann::json SchemaToJson() const;
};

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

 private:
  std::unique_ptr<duckdb::DuckDB> db_;
};

// Executes `sql` against a throwaway in-memory database and returns the first cell as text.
std::string ExecuteScalarString(const std::string& sql);

}  // namespace bigquery_emulator_duckdb
