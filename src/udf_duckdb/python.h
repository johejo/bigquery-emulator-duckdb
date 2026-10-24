#pragma once

#include "duckdb.h"
#include "src/udf/python/options.h"

namespace bigquery_emulator_duckdb::udf_duckdb {

void RegisterPythonFunctions(duckdb_connection connection, const udf::PythonOptions& options = {});

}  // namespace bigquery_emulator_duckdb::udf_duckdb
