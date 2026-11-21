#pragma once

#include "duckdb.h"

namespace bigquery_emulator_duckdb::duckdb_functions {

// Order-preserving FLOAT64 keys and their offset arithmetic for RANGE window frames.
void RegisterFloatRangeFunctions(duckdb_connection connection);

}  // namespace bigquery_emulator_duckdb::duckdb_functions
