#pragma once

#include "duckdb.h"

namespace duckdb {
// Callers of the C adapter need not parse DuckDB's C++ connection headers.
class Connection;
// Result readers keep their C API interface without importing C++ vector and type headers.
class DataChunk;
struct LogicalType;
}  // namespace duckdb

namespace bigquery_emulator_duckdb {

// Borrows a C API handle for existing function registrations. The handle
// must not be disconnected or retained beyond the C++ connection's lifetime.
duckdb_connection BorrowCConnection(duckdb::Connection& connection);

// Read-only views for C API result readers. Do not destroy or mutate the borrowed objects.
duckdb_logical_type BorrowCLogicalType(const duckdb::LogicalType& type);
duckdb_data_chunk BorrowCChunk(duckdb::DataChunk& chunk);

}  // namespace bigquery_emulator_duckdb
