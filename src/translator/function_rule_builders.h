#pragma once

#include <cstddef>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

#include "src/translator/functions.h"

namespace bigquery_emulator_duckdb::translator {

Condition Is(std::size_t argument, std::initializer_list<googlesql::TypeKind> types);
Condition Part(std::size_t argument, std::initializer_list<std::string_view> date_parts);
std::vector<Rule> Same(std::string_view function, Arity arity,
                       const std::vector<Condition>& conditions);
std::vector<Rule> Concat(std::initializer_list<std::vector<Rule>> groups);
std::string BigNumericCall(std::string_view function, std::size_t bignumerics,
                           std::size_t others = 0);
Rule BigNumericOperator(std::string_view function, std::size_t arity);

}  // namespace bigquery_emulator_duckdb::translator
