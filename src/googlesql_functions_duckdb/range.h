#pragma once

#include "duckdb.h"

namespace bigquery_emulator_duckdb::functions_duckdb {

void RegisterRangeFunctions(duckdb_connection connection);

}  // namespace bigquery_emulator_duckdb::functions_duckdb
