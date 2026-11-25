#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <utility>

#include "absl/strings/ascii.h"
#include "absl/strings/escaping.h"
#include "googlesql/public/constant.h"
#include "googlesql/public/function.h"
#include "googlesql/public/type.h"
#include "googlesql/public/value.h"
#include "googlesql/resolved_ast/resolved_ast.h"
#include "src/key_ranges.h"
#include "src/language_options.h"
#include "src/statement_resources.h"
#include "src/translator/collation.h"
#include "src/translator/context.h"
#include "src/translator/scan.h"

namespace bigquery_emulator_duckdb::translator {
namespace {

// A STRING or BYTES constant's bytes, with NULL as none; nullopt for anything else.
using Constant = std::optional<std::optional<std::string>>;

Constant ValueBytes(const googlesql::Value& value) {
  if (!value.type()->IsString() && !value.type()->IsBytes()) {
    return std::nullopt;
  }
  if (value.is_null()) {
    return std::optional<std::string>{};
  }
  return std::optional<std::string>{value.type()->IsString() ? value.string_value()
                                                             : value.bytes_value()};
}

// Literals, script variables and query parameters, which evaluate without errors.
Constant ConstantBytes(const googlesql::ResolvedExpr& expr, const Scope& scope) {
  if (!expr.type()->IsString() && !expr.type()->IsBytes()) {
    return std::nullopt;
  }
  if (expr.Is<googlesql::ResolvedLiteral>()) {
    return ValueBytes(expr.GetAs<googlesql::ResolvedLiteral>()->value());
  }
  if (expr.Is<googlesql::ResolvedConstant>()) {
    const auto value = expr.GetAs<googlesql::ResolvedConstant>()->constant()->GetValue();
    return value.ok() ? ValueBytes(*value) : std::nullopt;
  }
  if (expr.Is<googlesql::ResolvedParameter>() && scope.context.resources != nullptr &&
      scope.context.resources->query()) {
    const auto* parameter = expr.GetAs<googlesql::ResolvedParameter>();
    const std::string& literal = parameter->name().empty()
                                     ? scope.context.parameters.ByPosition(parameter->position())
                                     : scope.context.parameters.ByName(parameter->name());
    const auto rows = scope.context.resources->query()("SELECT hex(" + literal + ")");
    if (rows.size() != 1 || rows.front().size() != 1) {
      return std::nullopt;
    }
    if (!rows.front().front()) {
      return std::optional<std::string>{};
    }
    std::string bytes;
    if (!absl::HexStringToBytes(*rows.front().front(), &bytes)) {
      return std::nullopt;
    }
    return std::optional<std::string>{std::move(bytes)};
  }
  return std::nullopt;
}

bool IsKey(const googlesql::ResolvedExpr& expr, int column_id) {
  return expr.Is<googlesql::ResolvedColumnRef>() &&
         expr.GetAs<googlesql::ResolvedColumnRef>()->column().column_id() == column_id;
}

KeyRanges Point(std::string key) {
  return {
      KeyRange{
          .start = KeyBound{.key = key, .inclusive = true},
          .end = KeyBound{.key = std::move(key), .inclusive = true},
      },
  };
}

// The ranges of the key that rows satisfying `expr` have, or nullopt when it does not restrict
// them. A NULL constant leaves a comparison unrestricted, although it matches no rows.
std::optional<KeyRanges> Ranges(const googlesql::ResolvedExpr& expr, int column_id,
                                const Scope& scope) {
  if (expr.Is<googlesql::ResolvedLiteral>()) {
    const auto& value = expr.GetAs<googlesql::ResolvedLiteral>()->value();
    if (value.type()->IsBool() && !value.is_null() && !value.bool_value()) {
      return KeyRanges{};
    }
    return std::nullopt;
  }
  if (!expr.Is<googlesql::ResolvedFunctionCall>()) {
    return std::nullopt;
  }
  const auto& call = *expr.GetAs<googlesql::ResolvedFunctionCall>();
  if (!IsBuiltinFunction(*call.function())) {
    return std::nullopt;
  }
  const std::string name = absl::AsciiStrToLower(call.function()->Name());
  const auto& arguments = call.argument_list();
  if (name == "$and") {
    std::optional<KeyRanges> result;
    for (const auto& argument : arguments) {
      if (auto ranges = Ranges(*argument, column_id, scope)) {
        result = result ? IntersectKeyRanges(*result, *ranges) : std::move(ranges);
      }
    }
    return result;
  }
  if (name == "$or") {
    KeyRanges result;
    for (const auto& argument : arguments) {
      auto ranges = Ranges(*argument, column_id, scope);
      if (!ranges) {
        return std::nullopt;
      }
      result = UnionKeyRanges(std::move(result), *ranges);
    }
    return result;
  }
  // A collated comparison orders sort keys rather than the key's bytes.
  if (arguments.empty() || std::ranges::any_of(arguments, [](const auto& argument) {
        return HasCollation(argument->type_annotation_map());
      })) {
    return std::nullopt;
  }
  const bool key_first = IsKey(*arguments.front(), column_id);
  if (name == "$in" && key_first) {
    KeyRanges result;
    for (size_t i = 1; i < arguments.size(); ++i) {
      const auto constant = ConstantBytes(*arguments.at(i), scope);
      if (!constant) {
        return std::nullopt;
      }
      if (*constant) {
        result = UnionKeyRanges(std::move(result), Point(**constant));
      }
    }
    return result;
  }
  if (name == "$between" && key_first && arguments.size() == 3) {
    const auto low = ConstantBytes(*arguments.at(1), scope);
    const auto high = ConstantBytes(*arguments.at(2), scope);
    if (!low || !*low || !high || !*high) {
      return std::nullopt;
    }
    return NormalizeKeyRanges({
        KeyRange{
            .start = KeyBound{.key = **low, .inclusive = true},
            .end = KeyBound{.key = **high, .inclusive = true},
        },
    });
  }
  if (arguments.size() != 2) {
    return std::nullopt;
  }
  if (name == "starts_with") {
    const auto prefix = ConstantBytes(*arguments.at(1), scope);
    return key_first && prefix && *prefix ? std::optional(PrefixKeyRanges(**prefix)) : std::nullopt;
  }
  // The comparison as key <op> constant.
  const bool flipped = !key_first;
  if (flipped && !IsKey(*arguments.back(), column_id)) {
    return std::nullopt;
  }
  const auto constant = ConstantBytes(*arguments.at(flipped ? 0 : 1), scope);
  if (!constant || !*constant) {
    return std::nullopt;
  }
  std::string op = name;
  if (flipped) {
    op = name == "$less"               ? "$greater"
         : name == "$less_or_equal"    ? "$greater_or_equal"
         : name == "$greater"          ? "$less"
         : name == "$greater_or_equal" ? "$less_or_equal"
                                       : name;
  }
  std::string key = **constant;
  if (op == "$equal") {
    return Point(std::move(key));
  }
  if (op == "$less" || op == "$less_or_equal") {
    return NormalizeKeyRanges({
        KeyRange{
            .start = std::nullopt,
            .end = KeyBound{.key = std::move(key), .inclusive = op == "$less_or_equal"},
        },
    });
  }
  if (op == "$greater" || op == "$greater_or_equal") {
    return NormalizeKeyRanges({
        KeyRange{
            .start = KeyBound{.key = std::move(key), .inclusive = op == "$greater_or_equal"},
            .end = std::nullopt,
        },
    });
  }
  return std::nullopt;
}

}  // namespace

std::optional<KeyRanges> ScanKeyRanges(const googlesql::ResolvedTableScan& scan, int column_id,
                                       const Scope& scope) {
  const auto filters = scope.context.scan_filters.find(&scan);
  if (filters == scope.context.scan_filters.end()) {
    return std::nullopt;
  }
  std::optional<KeyRanges> result;
  for (const auto* filter : filters->second) {
    if (auto ranges = Ranges(*filter, column_id, scope)) {
      result = result ? IntersectKeyRanges(*result, *ranges) : std::move(ranges);
    }
  }
  return result;
}

}  // namespace bigquery_emulator_duckdb::translator
