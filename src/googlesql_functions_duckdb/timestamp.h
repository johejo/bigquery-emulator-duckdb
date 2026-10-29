#pragma once

#include "duckdb.h"

namespace bigquery_emulator_duckdb::functions_duckdb {

// Registers the epoch-nanosecond type, exact microsecond adapters, and a text view usable from
// shell --duckdb. Registered in both modes so explicit debug calls have the same spelling.
void RegisterTimestampNanosFunctions(duckdb_connection connection);

}  // namespace bigquery_emulator_duckdb::functions_duckdb
