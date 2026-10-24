#pragma once

#include "duckdb.h"
#include "src/googlesql_functions_duckdb/objectref_options.h"

namespace bigquery_emulator_duckdb::functions_duckdb {

// Registers OBJ checks and, when configured, external metadata fetches.
void RegisterObjectRefFunctions(duckdb_connection connection, const ObjectStorage& storage);

}  // namespace bigquery_emulator_duckdb::functions_duckdb
