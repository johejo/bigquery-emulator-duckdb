#include "src/duckdb_capi.h"

#include "duckdb.h"
#include "duckdb/common/types.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/main/connection.hpp"

namespace bigquery_emulator_duckdb {

duckdb_connection BorrowCConnection(duckdb::Connection& connection) {
  // DuckDB v2.0.0's V1 C API casts this handle directly to Connection* (see
  // src/main/capi/v1/duckdb-c.cpp). Keep that version-specific boundary here; ownership stays
  // with C++, and no DatabaseWrapper or other C API internals are needed.
  return reinterpret_cast<duckdb_connection>(&connection);
}

duckdb_logical_type BorrowCLogicalType(const duckdb::LogicalType& type) {
  // V1's logical type accessors take non-const handles even when they only read the type.
  return reinterpret_cast<duckdb_logical_type>(const_cast<duckdb::LogicalType*>(&type));
}

duckdb_data_chunk BorrowCChunk(duckdb::DataChunk& chunk) {
  return reinterpret_cast<duckdb_data_chunk>(&chunk);
}

}  // namespace bigquery_emulator_duckdb
