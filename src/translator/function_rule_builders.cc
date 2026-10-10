#include "src/translator/function_rule_builders.h"

#include <cstddef>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

#include "src/translator/functions.h"

namespace bigquery_emulator_duckdb::translator {

using enum googlesql::TypeKind;

Condition Is(std::size_t argument, std::initializer_list<googlesql::TypeKind> types) {
  return {.argument = argument, .types = types};
}

Condition Part(std::size_t argument, std::initializer_list<std::string_view> date_parts) {
  return {.argument = argument, .date_parts = date_parts};
}

// The DuckDB function of the same name, called with each argument count in `arity`, when the
// conditions hold.
std::vector<Rule> Same(std::string_view function, Arity arity,
                       const std::vector<Condition>& conditions) {
  std::vector<Rule> rules;
  for (std::size_t count = arity.min; count <= arity.max; ++count) {
    std::string spelling = std::string(function) + "(";
    for (std::size_t i = 1; i <= count; ++i) {
      spelling += (i == 1 ? "$" : ", $") + std::to_string(i);
    }
    rules.push_back({count, spelling + ")", conditions});
  }
  return rules;
}

std::vector<Rule> Concat(std::initializer_list<std::vector<Rule>> groups) {
  std::vector<Rule> rules;
  for (const auto& group : groups) {
    rules.insert(rules.end(), group.begin(), group.end());
  }
  return rules;
}

// A call to `function`, a BIGNUMERIC function that src/backend_functions/bignumeric.cc
// implements, which takes and returns the units of a BIGNUM as text. `bignumerics` arguments are
// BIGNUMERIC, and the `others` after them, such as ROUND's digits, go as they are.
std::string BigNumericCall(std::string_view function, std::size_t bignumerics, std::size_t others) {
  std::string arguments;
  for (std::size_t i = 1; i <= bignumerics + others; ++i) {
    const std::string argument = "$" + std::to_string(i);
    arguments +=
        (i == 1 ? "" : ", ") + (i <= bignumerics ? "CAST(" + argument + " AS VARCHAR)" : argument);
  }
  return "CAST(" + std::string(function) + "(" + arguments + ") AS BIGNUM)";
}

// An operator on BIGNUMERIC, all of whose `arity` arguments are BIGNUMERIC.
Rule BigNumericOperator(std::string_view function, std::size_t arity) {
  return {arity, BigNumericCall(function, arity), {Is(1, {TYPE_BIGNUMERIC})}};
}

}  // namespace bigquery_emulator_duckdb::translator
