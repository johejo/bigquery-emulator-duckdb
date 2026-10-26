#pragma once

#include "duckdb.h"

namespace bigquery_emulator_duckdb::functions_duckdb {

void RegisterContainsSubstr(duckdb_connection connection);

}  // namespace bigquery_emulator_duckdb::functions_duckdb
