#pragma once

#include <optional>
#include <string>

#include "duckdb.h"

namespace bigquery_emulator_duckdb {

// Registers GoogleSQL implementations and the configured execution identity in `database`;
// see src/translator/functions.cc. Throws BackendError when DuckDB rejects a function.
void RegisterBackendFunctions(duckdb_database database,
                              const std::optional<std::string>& session_user);

}  // namespace bigquery_emulator_duckdb
