#pragma once

#include "duckdb.h"

namespace bigquery_emulator_duckdb {

// Registers DuckDB's postgres_scanner extension, which is linked into the binary, in `database`.
// Throws BackendError when DuckDB rejects it.
void RegisterPostgresScanner(duckdb_database database);

}  // namespace bigquery_emulator_duckdb
