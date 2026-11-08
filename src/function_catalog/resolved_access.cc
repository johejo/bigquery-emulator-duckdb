#include "src/function_catalog/resolved_access.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

#include "googlesql/analyzer/expr_resolver_helper.h"
#include "googlesql/public/function.h"
#include "googlesql/public/type.h"
#include "googlesql/public/value.h"
#include "googlesql/resolved_ast/resolved_ast.h"
#include "src/duckdb_sql.h"
#include "src/function_catalog/catalog.h"

namespace bigquery_emulator_duckdb::function_catalog {

void RejectArgumentTypes(const googlesql::ResolvedFunctionCallBase& call,
                         std::span<const TypeRejection> rejections) {
  for (const auto& rejection : rejections) {
    if (rejection.types.size() > call.argument_list().size()) {
      continue;
    }
    bool matches = true;
    size_t i = 0;
    for (const auto type : rejection.types) {
      if (type != googlesql::TYPE_UNKNOWN &&
          type != call.argument_list(static_cast<int>(i))->type()->kind()) {
        matches = false;
        break;
      }
      ++i;
    }
    if (matches) {
      throw std::invalid_argument(std::string(rejection.message));
    }
  }
}

bool OrdersOnlyArgument(const googlesql::ResolvedAggregateFunctionCall& call, size_t argument) {
  if (argument >= call.argument_list().size() || call.order_by_item_list().size() != 1) {
    return false;
  }
  const auto* expression = call.argument_list(static_cast<int>(argument));
  return expression->Is<googlesql::ResolvedColumnRef>() &&
         expression->GetAs<googlesql::ResolvedColumnRef>()->column() ==
             call.order_by_item_list(0)->column_ref()->column();
}

bool IsConstant(const googlesql::ResolvedExpr& expr) {
  return expr.Is<googlesql::ResolvedLiteral>() || expr.Is<googlesql::ResolvedParameter>();
}

std::optional<bool> IsConstantExpression(const googlesql::ResolvedExpr& expr) {
  const auto constant = googlesql::IsConstantExpression(&expr);
  return constant.ok() ? std::optional<bool>(*constant) : std::nullopt;
}

bool IsNullLiteral(const googlesql::ResolvedExpr& expr) {
  return expr.Is<googlesql::ResolvedLiteral>() &&
         expr.GetAs<googlesql::ResolvedLiteral>()->value().is_null();
}

std::optional<std::string> StringLiteral(const googlesql::ResolvedExpr& expr) {
  if (!expr.Is<googlesql::ResolvedLiteral>()) {
    return std::nullopt;
  }
  const auto& value = expr.GetAs<googlesql::ResolvedLiteral>()->value();
  if (value.is_null() || !value.type()->IsString()) {
    return std::nullopt;
  }
  return value.string_value();
}

std::optional<std::string> DatePart(const googlesql::ResolvedExpr& expr) {
  if (!expr.Is<googlesql::ResolvedLiteral>()) {
    return std::nullopt;
  }
  const auto& value = expr.GetAs<googlesql::ResolvedLiteral>()->value();
  if (value.is_null() || !value.type()->IsEnum()) {
    return std::nullopt;
  }
  const std::string part = ToLowerAscii(value.EnumDisplayName());
  static constexpr auto supported = std::to_array<std::string_view>({
      "year",         "quarter",        "month",         "week",        "day",
      "hour",         "minute",         "second",        "millisecond", "microsecond",
      "isoyear",      "isoweek",        "dayofweek",     "dayofyear",   "week_monday",
      "week_tuesday", "week_wednesday", "week_thursday", "week_friday", "week_saturday",
  });
  return std::ranges::find(supported, part) != supported.end() ? std::optional<std::string>(part)
                                                               : std::nullopt;
}

std::optional<std::string> RoundingModeOf(const googlesql::ResolvedExpr& expr) {
  if (!expr.Is<googlesql::ResolvedLiteral>()) {
    return std::nullopt;
  }
  const auto& value = expr.GetAs<googlesql::ResolvedLiteral>()->value();
  if (value.is_null() || !value.type()->Equals(googlesql::types::RoundingModeEnumType())) {
    return std::nullopt;
  }
  return value.EnumDisplayName();
}

std::optional<std::string> NormalizeMode(const googlesql::ResolvedExpr& expr) {
  if (!expr.Is<googlesql::ResolvedLiteral>()) {
    return std::nullopt;
  }
  const auto& value = expr.GetAs<googlesql::ResolvedLiteral>()->value();
  if (value.is_null() || !value.type()->Equals(googlesql::types::NormalizeModeEnumType())) {
    return std::nullopt;
  }
  return value.EnumDisplayName();
}

const googlesql::ResolvedExpr& ScalarCall::Argument(size_t i) const {
  return *resolved.argument_list(static_cast<int>(i));
}

const googlesql::Type* ScalarCall::ArgumentType(size_t i) const { return Argument(i).type(); }

const googlesql::Type* ScalarCall::ResultType() const { return resolved.type(); }

}  // namespace bigquery_emulator_duckdb::function_catalog
