#include "src/function_catalog/catalog.h"

#include <algorithm>
#include <iterator>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "src/function_catalog/rules.h"

namespace bigquery_emulator_duckdb::function_catalog {
namespace {

std::vector<Implementation> Implementations(const std::vector<Rule>& rules,
                                            Implementation fallback) {
  std::set<Implementation> implementations;
  std::ranges::transform(
      rules, std::inserter(implementations, implementations.end()),
      [fallback](const Rule& rule) { return rule.implementation.value_or(fallback); });
  return {implementations.begin(), implementations.end()};
}

std::string_view Describe(Implementation implementation) {
  switch (implementation) {
    case Implementation::kSame:
      return "DuckDB function";
    case Implementation::kRenamed:
      return "DuckDB function, renamed";
    case Implementation::kSQL:
      return "DuckDB SQL";
    case Implementation::kGoogleSQL:
      return "GoogleSQL function";
    case Implementation::kS2:
      return "S2 function";
    case Implementation::kSQLInCode:
      return "DuckDB SQL, in code";
    case Implementation::kCustom:
      return "Emulator function";
    case Implementation::kSafe:
      return "SAFE. operator";
    case Implementation::kAggregate:
      return "DuckDB aggregate";
    case Implementation::kAnalytic:
      return "DuckDB window function";
  }
  return "";
}

}  // namespace

std::string Describe(const FunctionEntry& entry) {
  std::string description;
  for (const Implementation implementation : entry.implementations) {
    if (!description.empty()) {
      description += " / ";
    }
    description += Describe(implementation);
  }
  return description;
}

void Catalog::Add(std::string_view name, FunctionEntry entry) {
  if (!functions_.emplace(name, std::move(entry)).second) {
    throw std::logic_error("function " + std::string(name) + " is registered twice");
  }
}

void Catalog::AddRules(std::string_view name, const FunctionDefinition<Rule>& definition,
                       Translation translation, Implementation fallback) {
  Add(name, {
                .translation = translation,
                .uses = definition.uses,
                .implementations = Implementations(definition.rules, fallback),
                .rules = definition.rules,
            });
}

bool Catalog::HasTypeUse(std::string_view upper_name, TypeUse use) const {
  const FunctionEntry* entry = Find(upper_name);
  return entry != nullptr && entry->uses.Contains(use);
}

const FunctionEntry* Catalog::Find(std::string_view upper_name) const {
  const auto entry = functions_.find(upper_name);
  return entry == functions_.end() ? nullptr : &entry->second;
}

}  // namespace bigquery_emulator_duckdb::function_catalog
