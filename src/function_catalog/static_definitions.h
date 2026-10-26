#pragma once

#include <array>
#include <cstddef>
#include <string_view>
#include <tuple>
#include <utility>

#include "src/function_catalog/rules.h"

namespace bigquery_emulator_duckdb::function_catalog {

// Own the overload arrays separately from their views: a definition can move while a
// constexpr tuple is being constructed, so it must not contain a span into itself.
template <typename RuleType, std::size_t N>
struct StaticDefinition {
  std::string_view name;
  std::array<RuleType, N> rules;
  StaticFunctionDefinition<RuleType> options;
};

template <typename RuleType, std::size_t N>
constexpr auto Define(std::string_view name, std::array<RuleType, N> rules,
                      StaticFunctionDefinition<RuleType> options = {}) {
  return StaticDefinition<RuleType, N>{name, std::move(rules), options};
}

// Concrete element types let the compiler deduce the size of a braced rule list.
template <std::size_t N>
constexpr auto Define(std::string_view name, const StaticRule (&rules)[N],
                      StaticFunctionDefinition<StaticRule> options = {}) {
  return Define(name, std::to_array(rules), options);
}

template <std::size_t N>
constexpr auto Define(std::string_view name, const StaticAggregateRule (&rules)[N],
                      StaticFunctionDefinition<StaticAggregateRule> options = {}) {
  return Define(name, std::to_array(rules), options);
}

template <typename RuleType, std::size_t N>
constexpr StaticNamedDefinition<RuleType> DefinitionView(
    const StaticDefinition<RuleType, N>& definition) {
  auto options = definition.options;
  options.rules = definition.rules;
  return {definition.name, options};
}

// Call only after the backing tuple has reached its final, static storage location.
// Both the tuple and its metadata backing arrays must outlive these views.
template <typename... Definitions>
constexpr auto DefinitionViews(const std::tuple<Definitions...>& definitions) {
  return std::apply(
      [](const auto&... definition) { return std::array{DefinitionView(definition)...}; },
      definitions);
}

}  // namespace bigquery_emulator_duckdb::function_catalog
