#pragma once

#include "duckdb.h"
#include "src/googlesql_functions_duckdb/options.h"

namespace bigquery_emulator_duckdb::functions_duckdb {

void RegisterJsonFunctions(duckdb_connection connection, const Options& options = {});

}  // namespace bigquery_emulator_duckdb::functions_duckdb
