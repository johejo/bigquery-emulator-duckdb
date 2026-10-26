#pragma once

#include <span>

#include "duckdb.h"
#include "src/function_catalog/rules.h"
#include "src/googlesql_functions_duckdb/options.h"

namespace bigquery_emulator_duckdb::functions_duckdb {

// SQL translations from the same definitions that register the trigonometric implementations.
std::span<const function_catalog::NamedDefinition<function_catalog::Rule>> TrigonometricFunctions();

void RegisterMathFunctions(duckdb_connection connection, const Options& options = {});

}  // namespace bigquery_emulator_duckdb::functions_duckdb
