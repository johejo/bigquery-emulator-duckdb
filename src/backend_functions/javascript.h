#pragma once

#include "duckdb.h"
#include "src/backend_functions/javascript_limits.h"

namespace bigquery_emulator_duckdb::backend_functions {

void RegisterJavaScriptFunctions(duckdb_connection connection, const JavaScriptLimits& limits = {});

}  // namespace bigquery_emulator_duckdb::backend_functions
