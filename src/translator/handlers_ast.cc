#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "googlesql/public/function.h"
#include "googlesql/public/type.h"
#include "googlesql/public/value.h"
#include "googlesql/resolved_ast/resolved_ast.h"
#include "src/duckdb_sql.h"
#include "src/translator/functions.h"
#include "src/translator/handlers.h"

namespace bigquery_emulator_duckdb::translator {

const googlesql::ResolvedExpr& ScalarCall::Argument(size_t i) const {
  return *resolved.argument_list(static_cast<int>(i));
}

const googlesql::Type* ScalarCall::ArgumentType(size_t i) const { return Argument(i).type(); }

const googlesql::Type* ScalarCall::ResultType() const { return resolved.type(); }

std::optional<std::string> DatePart(const googlesql::ResolvedExpr& expr) {
  if (!expr.Is<googlesql::ResolvedLiteral>()) {
    return std::nullopt;
  }
  const auto& value = expr.GetAs<googlesql::ResolvedLiteral>()->value();
  if (value.is_null() || !value.type()->IsEnum()) {
    return std::nullopt;
  }
  const std::string part = ToLowerAscii(value.EnumDisplayName());
  static const std::set<std::string> supported = {
      "year",   "quarter",     "month",       "week",    "day",     "hour",      "minute",
      "second", "millisecond", "microsecond", "isoyear", "isoweek", "dayofweek", "dayofyear",
  };
  return supported.contains(part) ? std::optional<std::string>(part) : std::nullopt;
}

std::optional<BucketWidth> BucketWidthOf(const googlesql::ResolvedExpr& expr) {
  if (expr.Is<googlesql::ResolvedFunctionCall>()) {
    const auto* call = expr.GetAs<googlesql::ResolvedFunctionCall>();
    if (call->function()->Name() != "$interval" || call->argument_list_size() != 2) {
      return std::nullopt;
    }
    static const std::map<std::string, std::pair<std::string, int64_t>> units = {
        {"year", {"months", 12}},
        {"quarter", {"months", 3}},
        {"month", {"months", 1}},
        {"week", {"days", 7}},
        {"day", {"days", 1}},
        {"hour", {"micros", 3600000000}},
        {"minute", {"micros", 60000000}},
        {"second", {"micros", 1000000}},
        {"millisecond", {"micros", 1000}},
        {"microsecond", {"micros", 1}},
    };
    const auto part = DatePart(*call->argument_list(1));
    const auto unit = part ? units.find(*part) : units.end();
    if (unit == units.end()) {
      return std::nullopt;
    }
    return BucketWidth{
        .unit = unit->second.first,
        .count = call->argument_list(0),
        .factor = unit->second.second,
    };
  }
  if (!expr.Is<googlesql::ResolvedLiteral>()) {
    return std::nullopt;
  }
  const auto& value = expr.GetAs<googlesql::ResolvedLiteral>()->value();
  if (value.is_null() || !value.type()->IsInterval() ||
      value.interval_value().get_nano_fractions() != 0) {
    return std::nullopt;
  }
  // A zero width counts as days, which raises the error BigQuery raises for every input type.
  std::vector<BucketWidth> parts;
  for (const auto& [unit, count] : {
           std::pair<std::string, int64_t>{"months", value.interval_value().get_months()},
           {"days", value.interval_value().get_days()},
           {"micros", value.interval_value().get_micros()},
       }) {
    if (count != 0) {
      parts.push_back({.unit = unit, .factor = count});
    }
  }
  if (parts.size() > 1) {
    return std::nullopt;
  }
  return parts.empty() ? BucketWidth{.unit = "days", .factor = 0} : parts.at(0);
}

}  // namespace bigquery_emulator_duckdb::translator
