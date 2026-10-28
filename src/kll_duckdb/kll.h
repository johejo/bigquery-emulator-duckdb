#pragma once

#include "duckdb.h"

namespace bigquery_emulator_duckdb::kll_duckdb {

void RegisterKllFunctions(duckdb_connection connection);

}  // namespace bigquery_emulator_duckdb::kll_duckdb
