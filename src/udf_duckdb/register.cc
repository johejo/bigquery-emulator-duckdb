#include "src/udf_duckdb/register.h"

#include "duckdb.h"
#include "src/backend_initializer.h"
#include "src/udf/javascript/limits.h"
#include "src/udf/python/options.h"
#include "src/udf_duckdb/javascript.h"
#include "src/udf_duckdb/python.h"

namespace bigquery_emulator_duckdb::udf_duckdb {

BackendInitializer UdfInitializer(const udf::JavaScriptLimits& javascript,
                                  const udf::PythonOptions& python) {
  return [javascript, python = udf::PythonOptions(python)](duckdb_connection connection) {
    RegisterJavaScriptFunctions(connection, javascript);
    RegisterPythonFunctions(connection, python);
  };
}

}  // namespace bigquery_emulator_duckdb::udf_duckdb
