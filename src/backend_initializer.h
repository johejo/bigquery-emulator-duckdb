#pragma once

#include <functional>

#include "duckdb.h"

namespace bigquery_emulator_duckdb {

// Registers application-specific functions once on a new backend database. Functions registered
// on this connection are available to all sessions. The connection must not be retained.
using BackendInitializer = std::function<void(duckdb_connection)>;

}  // namespace bigquery_emulator_duckdb
