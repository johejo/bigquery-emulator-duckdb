#pragma once

#include <string_view>
#include <unordered_map>
#include <vector>

#include "src/translator/functions.h"

namespace bigquery_emulator_duckdb::translator {

const std::unordered_map<std::string_view, std::vector<AggregateRule>>& Aggregates();
const std::unordered_map<std::string_view, std::vector<AggregateRule>>& Analytics();

}  // namespace bigquery_emulator_duckdb::translator
