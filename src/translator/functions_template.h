#pragma once

#include <string_view>
#include <unordered_map>

#include "src/translator/functions.h"

namespace bigquery_emulator_duckdb::translator {

const std::unordered_map<std::string_view, FunctionDefinition<Rule>>& TemplateRules();

}  // namespace bigquery_emulator_duckdb::translator
