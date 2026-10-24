#pragma once

#include <optional>
#include <string>

#include "duckdb.h"
#include "src/googlesql_functions_duckdb/objectref_options.h"
#include "src/googlesql_geography/options.h"
#include "src/udf/javascript/limits.h"
#include "src/udf/python/options.h"

namespace bigquery_emulator_duckdb {

// Registers GoogleSQL implementations, the configured execution identity, JavaScript UDFs under
// `javascript` and the OBJ functions over `storage` in `database`; see
// src/translator/functions.cc. Geography bounds expand as in BigQuery unless
// geography.expand_bounding_box is false. Throws BackendError when DuckDB rejects a function.
void RegisterBackendFunctions(
    duckdb_database database, const std::optional<std::string>& session_user,
    const udf::JavaScriptLimits& javascript = udf::JavaScriptLimits(),
    const functions_duckdb::ObjectStorage& storage = functions_duckdb::ObjectStorage(),
    const udf::PythonOptions& python = udf::PythonOptions(),
    const googlesql_geography::Options& geography = {.expand_bounding_box = true});

}  // namespace bigquery_emulator_duckdb
