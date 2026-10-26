#pragma once

#include <span>

#include "src/function_catalog/rules.h"

namespace bigquery_emulator_duckdb::translator {

std::span<const function_catalog::StaticNamedDefinition<function_catalog::StaticRule>>
StaticBackendRules();

}  // namespace bigquery_emulator_duckdb::translator
