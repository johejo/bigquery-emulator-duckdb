#pragma once

// Rule builders and tables shared within the functions target. Public lookup and translation
// stay in functions.h; each function registration belongs to exactly one table.
#include <cstddef>
#include <initializer_list>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "src/translator/functions.h"

namespace bigquery_emulator_duckdb::translator {

Condition Is(std::size_t argument, std::initializer_list<googlesql::TypeKind> types);
Condition Part(std::size_t argument, std::initializer_list<std::string_view> date_parts);
Condition Mode(std::size_t argument, std::string_view rounding_mode);
std::vector<Rule> Same(std::string_view function, Arity arity,
                       const std::vector<Condition>& conditions);
std::vector<Rule> Concat(std::initializer_list<std::vector<Rule>> groups);
std::string BigNumericCall(std::string_view function, std::size_t bignumerics,
                           std::size_t others = 0);
Rule BigNumericOperator(std::string_view function, std::size_t arity);

const std::unordered_map<std::string_view, std::vector<Rule>>& TemplateRules();
const std::unordered_map<std::string_view, std::vector<Rule>>& BackendRules();
const std::unordered_map<std::string_view, std::vector<AggregateRule>>& Aggregates();
const std::unordered_map<std::string_view, std::vector<AggregateRule>>& Analytics();

}  // namespace bigquery_emulator_duckdb::translator
