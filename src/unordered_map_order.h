#pragma once

#include <string>
#include <vector>

namespace bigquery_emulator_duckdb {

// The order in which libc++'s std::unordered_map<std::string, T> on a 64-bit platform iterates
// `keys`, emplaced in their order; a repeated key keeps its first place. BigQuery reports the
// fields it detects in a JSON object in this order.
std::vector<std::string> UnorderedMapOrder(const std::vector<std::string>& keys);

}  // namespace bigquery_emulator_duckdb
