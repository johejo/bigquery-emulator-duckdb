#pragma once

#include <span>

#include "src/function_catalog/rules.h"

namespace bigquery_emulator_duckdb::geography_duckdb {

// Fixed SQL translations, backed by constexpr definitions.
std::span<const function_catalog::StaticNamedDefinition<function_catalog::StaticRule>>
GeographyFunctions();

// ST_UNION_AGG, ST_CENTROID_AGG and ST_EXTENT, whose finishes call the registered functions.
std::span<const function_catalog::StaticNamedDefinition<function_catalog::StaticAggregateRule>>
GeographyAggregates();

// Analytic functions that compute one result per geography in a partition.
std::span<const function_catalog::StaticNamedDefinition<function_catalog::StaticAggregateRule>>
GeographyAnalytics();

}  // namespace bigquery_emulator_duckdb::geography_duckdb
