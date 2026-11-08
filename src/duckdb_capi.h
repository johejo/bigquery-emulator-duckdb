#pragma once

#include "duckdb.h"

namespace duckdb {
// Callers of the C adapter need not parse DuckDB's C++ connection headers.
class Connection;
// Keep C++ type definitions out of C callback interfaces.
struct LogicalType;
}  // namespace duckdb

namespace bigquery_emulator_duckdb {

// Borrows a C API handle for existing function registrations. The handle
// must not be disconnected or retained beyond the C++ connection's lifetime.
duckdb_connection BorrowCConnection(duckdb::Connection& connection);

// Read-only access to a V1 callback's type. The C handle retains ownership.
const duckdb::LogicalType& BorrowCppLogicalType(duckdb_logical_type type);

}  // namespace bigquery_emulator_duckdb
