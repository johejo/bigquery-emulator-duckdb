#pragma once

#include <functional>

#include "duckdb.h"

namespace bigquery_emulator_duckdb {

// Registers application-specific functions once on a new backend database. Functions registered
// on this connection are available to all sessions. The connection must not be retained.
using BackendInitializer = std::function<void(duckdb_connection)>;

// Loads application-specific extensions once after common database setup. The database must not
// be retained. Runs before sessions are created, outside all query transactions.
using DatabaseInitializer = std::function<void(duckdb_database)>;

}  // namespace bigquery_emulator_duckdb
