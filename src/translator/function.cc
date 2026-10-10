#include "googlesql/public/function.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "googlesql/public/type.h"
#include "googlesql/public/value.h"
#include "googlesql/resolved_ast/resolved_ast.h"
#include "src/duckdb_sql.h"
#include "src/javascript_function.h"
#include "src/translator/context.h"
#include "src/translator/expression.h"
#include "src/translator/function.h"
#include "src/translator/functions.h"
#include "src/translator/handlers.h"
#include "src/type_mapping.h"

namespace bigquery_emulator_duckdb::translator {
namespace {

// The name of a rounding mode, such as ROUND_HALF_EVEN, which is also an enum literal after
// analysis.
std::optional<std::string> RoundingMode(const googlesql::ResolvedExpr& expr) {
  if (!expr.Is<googlesql::ResolvedLiteral>()) {
    return std::nullopt;
  }
  const auto& value = expr.GetAs<googlesql::ResolvedLiteral>()->value();
  if (value.is_null() || !value.type()->Equals(googlesql::types::RoundingModeEnumType())) {
    return std::nullopt;
  }
  return value.EnumDisplayName();
}

// The name of a NORMALIZE mode, such as NFKC.
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

// The DuckDB spelling of a scalar call over already translated arguments.
std::optional<std::string> Call(const googlesql::ResolvedFunctionCall& resolved,
                                std::string_view name, const std::vector<std::string>& args,
                                bool safe) {
  const FunctionEntry* entry = FindFunction(name);
  if (entry == nullptr) {
    return std::nullopt;
  }
  ScalarCall call{.resolved = resolved, .name = name, .safe = safe};
  for (size_t i = 0; i < args.size(); ++i) {
    const googlesql::ResolvedExpr& argument = *resolved.argument_list(static_cast<int>(i));
    call.arguments.push_back({
        .sql = args.at(i),
        .type = argument.type()->kind(),
        .date_part = DatePart(argument),
        .rounding_mode = RoundingMode(argument),
    });
  }
  if (entry->implementation == Implementation::kHandler) {
    return entry->handler(call);
  }
  return TranslateFunction(name, call.arguments, safe);
}

}  // namespace

bool HasBigNumeric(const googlesql::Type* type) {
  if (type->IsArray()) {
    return HasBigNumeric(type->AsArray()->element_type());
  }
  if (type->IsStruct()) {
    return std::ranges::any_of(type->AsStruct()->fields(), [](const googlesql::StructField& field) {
      return HasBigNumeric(field.type);
    });
  }
  return type->IsBigNumericType();
}

bool InvolvesBigNumeric(const googlesql::ResolvedFunctionCallBase& call) {
  return HasBigNumeric(call.type()) ||
         std::ranges::any_of(call.argument_list(),
                             [](const auto& argument) { return HasBigNumeric(argument->type()); });
}

namespace {

// A call to a JavaScript UDF, which the backend function for its result type runs on `source`.
std::optional<std::string> JavaScriptCall(const googlesql::ResolvedFunctionCall& call,
                                          const std::string& source, const Scope& scope,
                                          const Columns& columns) {
  const std::string name = call.function()->Name();
  if (call.error_mode() != googlesql::ResolvedFunctionCallBase::DEFAULT_ERROR_MODE) {
    return Unsupported(scope, "SAFE. calls of JavaScript UDFs");
  }
  const googlesql::Type* type = call.type();
  const char* function = type->IsBool()     ? "bq_js_bool"
                         : type->IsDouble() ? "bq_js_double"
                         : type->IsString() ? "bq_js_string"
                         : type->IsInt64()  ? "bq_js_int64"
                                            : nullptr;
  if (function == nullptr) {
    return Unsupported(scope, "JavaScript UDF " + name + " result type");
  }
  std::vector<std::string> args = {QuoteLiteral(source)};
  for (const auto& argument : call.argument_list()) {
    const auto sql = Expression(*argument, scope, columns);
    const auto duckdb_type = DuckDbType(argument->type());
    if (!sql || !duckdb_type) {
      return std::nullopt;
    }
    args.push_back("CAST(" + *sql + " AS " + *duckdb_type + ")");
  }
  return std::string(function) + "(" + Join(args, ", ") + ")";
}

}  // namespace

std::optional<std::string> Function(const googlesql::ResolvedFunctionCall& call, const Scope& scope,
                                    const Columns& columns) {
  const std::string name = ToUpperAscii(call.function()->Name());
  if (!call.generic_argument_list().empty() || !call.hint_list().empty() ||
      !call.collation_list().empty()) {
    return Unsupported(scope, "function " + name + " with generic arguments, hints or collation");
  }
  if (const auto* javascript = dynamic_cast<const JavaScriptFunction*>(call.function())) {
    return JavaScriptCall(call, javascript->source(), scope, columns);
  }
  // CONTAINS_SUBSTR is supplied by our catalog because GoogleSQL lacks this BigQuery builtin.
  if (!call.function()->IsGoogleSQLBuiltin() && name != "CONTAINS_SUBSTR") {
    return Unsupported(scope, "function " + name);
  }
  // BigQuery rejects this prefix although GoogleSQL accepts it.
  if (name == "SESSION_USER" &&
      call.error_mode() == googlesql::ResolvedFunctionCallBase::SAFE_ERROR_MODE) {
    throw std::invalid_argument("SAFE with function session_user is not supported.");
  }
  if (name == "SESSION_USER" && !scope.context.defaults.has_session_user) {
    return Unsupported(scope, "function SESSION_USER");
  }
  const FunctionEntry* entry = FindFunction(name);
  if (entry == nullptr) {
    return Unsupported(scope, "function " + name);
  }
  const bool alias = entry->implementation == Implementation::kSafe;
  const std::string function = alias ? std::string(entry->target) : name;
  const bool safe =
      call.error_mode() == googlesql::ResolvedFunctionCallBase::SAFE_ERROR_MODE || alias;
  if (!safe && call.error_mode() != googlesql::ResolvedFunctionCallBase::DEFAULT_ERROR_MODE) {
    return Unsupported(scope, "function " + name + " error mode");
  }
  if (!SupportsBigNumeric(function) && InvolvesBigNumeric(call)) {
    return Unsupported(scope, "function " + name + " with BIGNUMERIC");
  }
  const bool interval = HasInterval(call.type()) ||
                        std::ranges::any_of(call.argument_list(), [](const auto& argument) {
                          return HasInterval(argument->type());
                        });
  // Enabling a physical type must not silently enable arithmetic, comparisons or JSON conversion.
  static constexpr std::string_view interval_functions[] = {
      "$INTERVAL",
      "$EXTRACT",
      "MAKE_INTERVAL",
      "JUSTIFY_HOURS",
      "JUSTIFY_DAYS",
      "JUSTIFY_INTERVAL",
      "$IS_NULL",
      "$CASE_NO_VALUE",
      "IF",
      "IFNULL",
      "COALESCE",
      "ERROR",
      "$MAKE_ARRAY",
      "$ARRAY_AT_OFFSET",
      "$ARRAY_AT_ORDINAL",
      "$SAFE_ARRAY_AT_OFFSET",
      "$SAFE_ARRAY_AT_ORDINAL",
      "ARRAY_LENGTH",
      "ARRAY_REVERSE",
      "ARRAY_CONCAT",
      "JUSTIFY_HOURS",
      "JUSTIFY_DAYS",
      "JUSTIFY_INTERVAL",
      "$IS_NULL",
      "$CASE_NO_VALUE",
      "IF",
      "IFNULL",
      "COALESCE",
      "ERROR",
      "$MAKE_ARRAY",
      "$ARRAY_AT_OFFSET",
      "$ARRAY_AT_ORDINAL",
      "$SAFE_ARRAY_AT_OFFSET",
      "$SAFE_ARRAY_AT_ORDINAL",
      "ARRAY_LENGTH",
      "ARRAY_REVERSE",
      "ARRAY_CONCAT",
      "JSON_ARRAY_APPEND",
      "JSON_ARRAY_INSERT",
  };
  if (interval && entry->handler != Bucket &&
      std::ranges::find(interval_functions, function) == std::end(interval_functions)) {
    return Unsupported(scope, "function " + name + " with INTERVAL");
  }
  // A RANGE is a STRUCT in DuckDB, which these compare and carry as BigQuery does a RANGE; others,
  // such as JSON conversion and FORMAT, would see the STRUCT.
  static constexpr std::string_view range_functions[] = {
      "RANGE",
      "RANGE_START",
      "RANGE_END",
      "RANGE_CONTAINS",
      "RANGE_OVERLAPS",
      "RANGE_INTERSECT",
      "$EQUAL",
      "$NOT_EQUAL",
      "$LESS",
      "$LESS_OR_EQUAL",
      "$GREATER",
      "$GREATER_OR_EQUAL",
      "$BETWEEN",
      "$IS_DISTINCT_FROM",
      "$IS_NOT_DISTINCT_FROM",
      "$IN",
      "$IN_ARRAY",
      "$CASE_WITH_VALUE",
      "NULLIF",
      "GREATEST",
      "LEAST",
      "$IS_NULL",
      "$CASE_NO_VALUE",
      "IF",
      "IFNULL",
      "COALESCE",
      "ERROR",
      "$MAKE_ARRAY",
      "$ARRAY_AT_OFFSET",
      "$ARRAY_AT_ORDINAL",
      "$SAFE_ARRAY_AT_OFFSET",
      "$SAFE_ARRAY_AT_ORDINAL",
      "ARRAY_LENGTH",
      "ARRAY_REVERSE",
      "ARRAY_CONCAT",
  };
  if ((HasRange(call.type()) ||
       std::ranges::any_of(call.argument_list(),
                           [](const auto& argument) { return HasRange(argument->type()); })) &&
      std::ranges::find(range_functions, function) == std::end(range_functions)) {
    return Unsupported(scope, "function " + name + " with RANGE");
  }
  // Supporting a new physical STRUCT shape does not make DuckDB's composite comparisons
  // compatible: its equality treats NULL fields as values instead of propagating NULL. Keep
  // these newly representable forms unsupported, including the implicit comparisons in CASE,
  // NULLIF and IN, until their semantics are implemented.
  static constexpr std::array<std::string_view, 12> comparisons = {
      "$EQUAL",         "$NOT_EQUAL",        "$LESS",
      "$LESS_OR_EQUAL", "$GREATER",          "$GREATER_OR_EQUAL",
      "$BETWEEN",       "$IS_DISTINCT_FROM", "$IN",
      "$IN_ARRAY",      "$CASE_WITH_VALUE",  "NULLIF",
  };
  const auto compares_internal_struct = [&] {
    for (int i = 0; i < call.argument_list_size(); ++i) {
      // CASE's THEN and ELSE values are results, not operands of its comparison.
      if (function == "$CASE_WITH_VALUE" &&
          (i == call.argument_list_size() - 1 || (i > 0 && i % 2 == 0))) {
        continue;
      }
      if (HasInternalStructNames(call.argument_list(i)->type())) {
        return true;
      }
    }
    return false;
  };
  if (std::ranges::find(comparisons, function) != comparisons.end() && compares_internal_struct()) {
    return Unsupported(scope, "comparison with anonymous or duplicate STRUCT fields");
  }
  const bool bucket = entry->handler == Bucket;
  std::vector<std::string> args;
  for (const auto& argument : call.argument_list()) {
    // The bucket width INTERVAL becomes a plain count, since DuckDB intervals keep no single
    // part to count in.
    if (bucket && argument->type()->IsInterval()) {
      const auto width = BucketWidthOf(*argument);
      if (!width) {
        return Unsupported(scope, "function " + name + " bucket width");
      }
      if (width->count == nullptr) {
        args.push_back(std::to_string(width->factor));
        continue;
      }
      const auto count = Expression(*width->count, scope, columns);
      if (!count) {
        return std::nullopt;
      }
      args.push_back("(" + *count + " * " + std::to_string(width->factor) + ")");
      continue;
    }
    if (argument->type()->IsEnum()) {
      if (const auto mode = RoundingMode(*argument)) {
        args.push_back(QuoteLiteral(*mode));
        continue;
      }
      if (const auto mode = NormalizeMode(*argument)) {
        args.push_back(QuoteLiteral(*mode));
        continue;
      }
      const auto part = DatePart(*argument);
      if (!part) {
        return Unsupported(scope, "function " + name + " date part");
      }
      args.push_back(QuoteLiteral(*part));
      continue;
    }
    const auto sql = Expression(*argument, scope, columns);
    if (!sql) {
      return std::nullopt;
    }
    args.push_back(*sql);
  }
  if (!safe) {
    auto sql = Call(call, function, args, false);
    if (!sql) {
      return Unsupported(scope, "function " + name);
    }
    return sql;
  }
  // SAFE. turns errors of the function itself into NULL, while errors evaluating its
  // arguments still propagate. The translation raises its own errors as NULL, and TRY turns
  // DuckDB's errors into NULL. The arguments are bound outside the lambda so TRY only covers the
  // call; binding them also keeps DuckDB from raising constant errors at bind time. DuckDB's TRY
  // rejects volatile functions, which GoogleSQL marks as not immutable.
  if (call.function()->function_options().volatility != googlesql::FunctionEnums::IMMUTABLE) {
    return Unsupported(scope, "SAFE." + name);
  }
  const std::string lambda = scope.context.FreshName("_s");
  std::vector<std::string> bound;
  std::vector<std::string> placeholders;
  for (size_t i = 0; i < args.size(); ++i) {
    if (call.argument_list(static_cast<int>(i))->type()->IsEnum()) {
      placeholders.push_back(args.at(i));
      continue;
    }
    const std::string field = "a" + std::to_string(i + 1);
    bound.push_back(field + " := " + args.at(i));
    placeholders.push_back(lambda);
    placeholders.back() += "." + field;
  }
  const auto sql = Call(call, function, placeholders, true);
  if (!sql) {
    return Unsupported(scope, "SAFE." + name);
  }
  if (bound.empty()) {
    return "TRY(" + *sql + ")";
  }
  return "list_transform([struct_pack(" + Join(bound, ", ") + ")], " + lambda + " -> TRY(" + *sql +
         "))[1]";
}

}  // namespace bigquery_emulator_duckdb::translator
