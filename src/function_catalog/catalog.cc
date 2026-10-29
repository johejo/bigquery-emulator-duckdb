#include "src/function_catalog/catalog.h"

#include <algorithm>
#include <iterator>
#include <optional>
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

void Catalog::Remove(std::string_view name) { functions_.erase(name); }

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

void Catalog::AddRules(std::string_view name, FunctionDefinition<Rule> definition,
                       Implementation fallback) {
  Add(name, {
                .translation = Translation::kRules,
                .uses = definition.uses,
                .implementations = Implementations(definition.rules, fallback),
                .rules = std::move(definition.rules),
                .argument_modes = std::move(definition.argument_modes),
                .struct_handler = definition.struct_handler,
                .type_rejections = std::move(definition.type_rejections),
            });
}

void Catalog::AddRules(std::string_view name,
                       const StaticFunctionDefinition<StaticRule>& definition,
                       Implementation fallback) {
  FunctionDefinition<Rule> owned{
      .uses = definition.uses,
      .argument_modes = {definition.argument_modes.begin(), definition.argument_modes.end()},
      .struct_handler = definition.struct_handler,
      .type_rejections = {definition.type_rejections.begin(), definition.type_rejections.end()},
  };
  owned.rules.reserve(definition.rules.size());
  for (const auto& rule : definition.rules) {
    owned.rules.push_back(Own(rule));
  }
  AddRules(name, std::move(owned), fallback);
}

void Catalog::AddAggregates(std::string_view name,
                            const StaticFunctionDefinition<StaticAggregateRule>& definition,
                            Translation translation, std::optional<Implementation> backend) {
  FunctionEntry entry{
      .translation = translation,
      .uses = definition.uses,
      .implementations =
          {
              translation == Translation::kAnalytic ? Implementation::kAnalytic
                                                    : Implementation::kAggregate,
          },
      .argument_modes = {definition.argument_modes.begin(), definition.argument_modes.end()},
      .struct_handler = definition.struct_handler,
      .type_rejections = {definition.type_rejections.begin(), definition.type_rejections.end()},
  };
  if (backend) {
    entry.implementations.push_back(*backend);
  }
  entry.aggregates.reserve(definition.rules.size());
  for (const auto& rule : definition.rules) {
    entry.aggregates.push_back(Own(rule));
  }
  Add(name, std::move(entry));
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
