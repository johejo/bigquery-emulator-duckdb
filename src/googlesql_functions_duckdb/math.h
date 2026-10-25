#pragma once

#include <string_view>
#include <unordered_map>

#include "duckdb.h"
#include "src/function_catalog/rules.h"

namespace bigquery_emulator_duckdb::functions_duckdb {

// SQL translations from the same definitions that register the trigonometric implementations.
const std::unordered_map<std::string_view,
                         function_catalog::FunctionDefinition<function_catalog::Rule>>&
TrigonometricFunctions();

void RegisterMathFunctions(duckdb_connection connection);

}  // namespace bigquery_emulator_duckdb::functions_duckdb
