#pragma once

#include <optional>
#include <string>

#include "duckdb.h"
#include "src/backend_functions/javascript_limits.h"

namespace bigquery_emulator_duckdb {

// Registers GoogleSQL implementations, the configured execution identity and JavaScript UDFs under
// `javascript` in `database`; see src/translator/functions.cc. Throws BackendError when DuckDB
// rejects a function.
void RegisterBackendFunctions(duckdb_database database,
                              const std::optional<std::string>& session_user,
                              const backend_functions::JavaScriptLimits& javascript = {});

}  // namespace bigquery_emulator_duckdb
