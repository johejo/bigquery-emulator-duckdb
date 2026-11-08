#pragma once

#include "duckdb.h"

namespace duckdb {
// Callers of the C adapter need not parse DuckDB's C++ connection headers.
class Connection;
}  // namespace duckdb

namespace bigquery_emulator_duckdb {

// Borrows a C API handle for existing function registrations and result readers. The handle
// must not be disconnected or retained beyond the C++ connection's lifetime.
duckdb_connection BorrowCConnection(duckdb::Connection& connection);

}  // namespace bigquery_emulator_duckdb
