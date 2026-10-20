#pragma once

#include "duckdb.h"

namespace bigquery_emulator_duckdb::backend_functions {

void RegisterHllCountFunctions(duckdb_connection connection);

}  // namespace bigquery_emulator_duckdb::backend_functions
