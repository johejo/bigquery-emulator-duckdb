#pragma once

#include <string_view>
#include <unordered_map>

#include "src/function_catalog/rules.h"

namespace bigquery_emulator_duckdb::translator {

const std::unordered_map<std::string_view,
                         function_catalog::FunctionDefinition<function_catalog::Rule>>&
TemplateRules();

}  // namespace bigquery_emulator_duckdb::translator
