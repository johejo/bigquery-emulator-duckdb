#pragma once

#include "duckdb.h"
#include "src/googlesql_functions_duckdb/options.h"

namespace bigquery_emulator_duckdb::functions_duckdb {

void RegisterStringFunctions(duckdb_connection connection, const Options& options = {});

}  // namespace bigquery_emulator_duckdb::functions_duckdb
