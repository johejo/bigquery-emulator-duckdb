#pragma once

#include <string_view>
#include <unordered_map>

#include "src/function_catalog/rules.h"

namespace bigquery_emulator_duckdb::geography_duckdb {

// SQL translations from the same definitions that register the DuckDB implementations.
const std::unordered_map<std::string_view,
                         function_catalog::FunctionDefinition<function_catalog::Rule>>&
GeographyFunctions();

// ST_UNION_AGG, ST_CENTROID_AGG and ST_EXTENT, whose finishes call the registered functions.
const std::unordered_map<std::string_view,
                         function_catalog::FunctionDefinition<function_catalog::AggregateRule>>&
GeographyAggregates();

}  // namespace bigquery_emulator_duckdb::geography_duckdb
