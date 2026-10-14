#pragma once

#include <string_view>
#include <unordered_map>
#include <vector>

#include "src/translator/functions.h"

namespace bigquery_emulator_duckdb::translator {

const std::unordered_map<std::string_view, std::vector<Rule>>& BackendRules();

// GEOGRAPHY functions, which GoogleSQL does not implement, over the S2 Geometry library.
const std::unordered_map<std::string_view, std::vector<Rule>>& S2Rules();

}  // namespace bigquery_emulator_duckdb::translator
