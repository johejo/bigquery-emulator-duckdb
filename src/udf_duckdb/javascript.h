#pragma once

#include "duckdb.h"
#include "src/udf/javascript/limits.h"

namespace bigquery_emulator_duckdb::udf_duckdb {

void RegisterJavaScriptFunctions(duckdb_connection connection,
                                 const udf::JavaScriptLimits& limits = {});

}  // namespace bigquery_emulator_duckdb::udf_duckdb
