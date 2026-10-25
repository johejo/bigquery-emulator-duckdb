#pragma once

#include <cstddef>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

#include "src/function_catalog/rules.h"

namespace bigquery_emulator_duckdb::function_catalog {

Condition Is(std::size_t argument, std::initializer_list<googlesql::TypeKind> types);
Condition Part(std::size_t argument, std::initializer_list<std::string_view> date_parts);
std::vector<Rule> Same(std::string_view function, Arity arity,
                       const std::vector<Condition>& conditions);
std::vector<Rule> Concat(std::initializer_list<std::vector<Rule>> groups);
std::string BigNumericCall(std::string_view function, std::size_t bignumerics,
                           std::size_t others = 0);
Rule BigNumericOperator(std::string_view function, std::size_t arity);
// `left` `op` `right`, where a NaN FLOAT64 compares unequal to everything, itself included, and
// neither less nor greater. DuckDB takes NaN as equal to itself and greater than every number.
std::string FloatCompare(const std::string& left, std::string_view op, const std::string& right);

}  // namespace bigquery_emulator_duckdb::function_catalog
