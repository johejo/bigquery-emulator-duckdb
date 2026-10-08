#pragma once

#include "duckdb.h"

namespace bigquery_emulator_duckdb::backend_functions {

void RegisterStringFunctions(duckdb_connection connection);

}  // namespace bigquery_emulator_duckdb::backend_functions
