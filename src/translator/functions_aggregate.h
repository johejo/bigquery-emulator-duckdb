#pragma once

#include <span>

#include "src/function_catalog/rules.h"

namespace bigquery_emulator_duckdb::translator {

std::span<const function_catalog::NamedDefinition<function_catalog::AggregateRule>> Aggregates();
std::span<const function_catalog::StaticNamedDefinition<function_catalog::StaticAggregateRule>>
Analytics();

}  // namespace bigquery_emulator_duckdb::translator
