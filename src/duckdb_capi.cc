#include "src/duckdb_capi.h"

#include "duckdb.h"
#include "duckdb/common/types.hpp"
#include "duckdb/main/connection.hpp"

namespace bigquery_emulator_duckdb {

duckdb_connection BorrowCConnection(duckdb::Connection& connection) {
  // DuckDB v2.0.0's V1 C API casts this handle directly to Connection* (see
  // src/main/capi/v1/duckdb-c.cpp). Keep that version-specific boundary here; ownership stays
  // with C++, and no DatabaseWrapper or other C API internals are needed.
  return reinterpret_cast<duckdb_connection>(&connection);
}

const duckdb::LogicalType& BorrowCppLogicalType(duckdb_logical_type type) {
  // V1 logical type handles point directly to LogicalType; ownership stays with the callback.
  return *reinterpret_cast<const duckdb::LogicalType*>(type);
}

}  // namespace bigquery_emulator_duckdb
