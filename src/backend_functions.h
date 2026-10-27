#pragma once

#include <optional>
#include <string>

#include "duckdb.h"
#include "src/backend_initializer.h"
#include "src/function_options.h"
#include "src/googlesql_functions_duckdb/objectref_options.h"
#include "src/googlesql_functions_duckdb/options.h"
#include "src/googlesql_geography/options.h"

namespace bigquery_emulator_duckdb {

// Registers GoogleSQL implementations, the configured execution identity and the OBJ functions
// over `storage` in `database`, then calls the optional application initializer; see
// src/translator/functions.cc. Geography bounds expand as in BigQuery unless
// geography.expand_bounding_box is false. Function corrections default to BigQuery policy.
// Throws BackendError when DuckDB rejects a function.
void RegisterBackendFunctions(
    duckdb_database database, const std::optional<std::string>& session_user,
    const functions_duckdb::ObjectStorage& storage = functions_duckdb::ObjectStorage(),
    const googlesql_geography::Options& geography = {.expand_bounding_box = true},
    const functions_duckdb::Options& functions = BigQueryFunctionOptions(),
    const BackendInitializer& initialize = {});

}  // namespace bigquery_emulator_duckdb
