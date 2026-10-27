#pragma once

#include "src/backend_initializer.h"
#include "src/udf/javascript/limits.h"
#include "src/udf/python/options.h"

namespace bigquery_emulator_duckdb::udf_duckdb {

// Composes the application's UDF runtimes without coupling the common backend to them.
BackendInitializer UdfInitializer(const udf::JavaScriptLimits& javascript,
                                  const udf::PythonOptions& python);

}  // namespace bigquery_emulator_duckdb::udf_duckdb
