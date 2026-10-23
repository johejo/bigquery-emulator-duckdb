#pragma once

#include <optional>
#include <string>

#include "duckdb.h"
#include "src/backend_functions/javascript_limits.h"
#include "src/backend_functions/object_metadata.h"
#include "src/backend_functions/python.h"

namespace bigquery_emulator_duckdb {

// Registers GoogleSQL implementations, the configured execution identity, JavaScript UDFs under
// `javascript` and the OBJ functions over `storage` in `database`; see
// src/translator/functions.cc. Throws BackendError when DuckDB rejects a function.
void RegisterBackendFunctions(
    duckdb_database database, const std::optional<std::string>& session_user,
    const backend_functions::JavaScriptLimits& javascript = backend_functions::JavaScriptLimits(),
    const backend_functions::ObjectStorage& storage = backend_functions::ObjectStorage(),
    const backend_functions::PythonOptions& python = {});

}  // namespace bigquery_emulator_duckdb
