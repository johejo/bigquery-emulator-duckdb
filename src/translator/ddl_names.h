#pragma once

#include <optional>
#include <string>
#include <vector>

namespace bigquery_emulator_duckdb::translator {
struct Scope;

std::optional<std::string> TargetTable(const std::vector<std::string>& path, const Scope& scope,
                                       bool create = false);
std::optional<std::string> TargetDataset(const std::vector<std::string>& path, const Scope& scope);

}  // namespace bigquery_emulator_duckdb::translator
