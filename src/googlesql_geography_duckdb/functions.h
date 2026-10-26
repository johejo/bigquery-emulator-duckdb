#pragma once

#include <span>
#include <string_view>
#include <unordered_map>

#include "src/function_catalog/rules.h"

namespace bigquery_emulator_duckdb::geography_duckdb {

// Fixed SQL translations, backed by constexpr definitions.
std::span<const function_catalog::StaticNamedDefinition<function_catalog::StaticRule>>
StaticGeographyFunctions();

// Runtime-generated translations from the definitions that register the DuckDB implementations.
const std::unordered_map<std::string_view,
                         function_catalog::FunctionDefinition<function_catalog::Rule>>&
GeographyFunctions();

// ST_UNION_AGG, ST_CENTROID_AGG and ST_EXTENT, whose finishes call the registered functions.
std::span<const function_catalog::StaticNamedDefinition<function_catalog::StaticAggregateRule>>
GeographyAggregates();

}  // namespace bigquery_emulator_duckdb::geography_duckdb
