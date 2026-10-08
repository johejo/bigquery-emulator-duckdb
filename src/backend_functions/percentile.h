#pragma once

#include "duckdb.h"

namespace bigquery_emulator_duckdb::backend_functions {

void RegisterPercentileFunctions(duckdb_connection connection);

}  // namespace bigquery_emulator_duckdb::backend_functions
