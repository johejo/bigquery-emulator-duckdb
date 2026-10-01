#pragma once

#include "duckdb.h"

namespace bigquery_emulator_duckdb {

// Registers the functions the translator calls beyond DuckDB's own, which GoogleSQL implements,
// in `database`; see src/functions.cc. Throws BackendError when DuckDB rejects one.
void RegisterBackendFunctions(duckdb_database database);

}  // namespace bigquery_emulator_duckdb
