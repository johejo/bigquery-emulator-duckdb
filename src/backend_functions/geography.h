#pragma once

#include "duckdb.h"

namespace bigquery_emulator_duckdb::backend_functions {

void RegisterGeographyFunctions(duckdb_connection connection);

}  // namespace bigquery_emulator_duckdb::backend_functions
