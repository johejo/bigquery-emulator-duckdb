#pragma once

#include "duckdb.h"

namespace bigquery_emulator_duckdb::hll_duckdb {

void RegisterHllCountFunctions(duckdb_connection connection);

}  // namespace bigquery_emulator_duckdb::hll_duckdb
