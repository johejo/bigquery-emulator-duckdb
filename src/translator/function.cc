#include "googlesql/public/function.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <iterator>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "googlesql/public/type.h"
#include "googlesql/public/value.h"
#include "googlesql/resolved_ast/resolved_ast.h"
#include "src/catalog.h"
#include "src/duckdb_sql.h"
#include "src/field_schema.h"
#include "src/javascript_function.h"
#include "src/translator/context.h"
#include "src/translator/expression.h"
#include "src/translator/function.h"
#include "src/translator/functions.h"
#include "src/translator/handlers.h"
#include "src/translator/resolved_access.h"
#include "src/type_mapping.h"

namespace bigquery_emulator_duckdb::translator {
namespace {

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
  static constexpr std::array<std::string_view, 6> struct_comparisons = {
      "$EQUAL", "$NOT_EQUAL", "$IN", "$IN_ARRAY", "$CASE_WITH_VALUE", "NULLIF",
  };
  if (!args.empty() && resolved.argument_list(0)->type()->IsStruct() &&
      std::ranges::find(struct_comparisons, name) != struct_comparisons.end()) {
    return StructComparison(call);
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

std::string GeographyWkt(const std::string& value, const googlesql::Type* type, int depth) {
  if (!HasGeography(type)) {
    return value;
  }
  if (type->IsGeography()) {
    return "bq_st_astext(" + value + ")";
  }
  // Each level binds its value once, under a name of its own.
  const std::string name = "_geo" + std::to_string(depth);
  if (type->IsArray()) {
    return "list_transform(" + value + ", " + name + " -> " +
           GeographyWkt(name, type->AsArray()->element_type(), depth + 1) + ")";
  }
  const auto* structure = type->AsStruct();
  const std::vector<std::string> names = DuckDbStructFieldNames(structure);
  std::vector<std::string> fields;
  fields.reserve(names.size());
  for (int i = 0; i < structure->num_fields(); ++i) {
    fields.push_back(QuoteIdentifier(names.at(i)) + " := " +
                     GeographyWkt("struct_extract_at(" + name + ", " + std::to_string(i + 1) + ")",
                                  structure->field(i).type, depth + 1));
  }
  return "list_transform([" + value + "], " + name + " -> CASE WHEN " + name +
         " IS NULL THEN NULL ELSE struct_pack(" + Join(fields, ", ") + ") END)[1]";
}

namespace {

// Bind each container once: rebuilding a STRUCT must not evaluate its expression per field.
std::optional<std::string> RoundedValue(const std::string& value, const googlesql::Type* type,
                                        const FieldSchema& field, std::string_view column,
                                        int depth) {
  if (!RoundsToEven(field)) {
    return value;
  }
  const std::string name = "_round" + std::to_string(depth);
  if (type->IsArray()) {
    const auto element =
        RoundedValue(name, type->AsArray()->element_type(), field, column, depth + 1);
    if (!element) {
      return std::nullopt;
    }
    return "coalesce(list_transform(" + value + ", " + name + " -> CASE WHEN " + name +
           " IS NULL THEN error(" +
           QuoteLiteral("Array cannot have a null element; error in writing field " +
                        std::string(column)) +
           ") ELSE " + *element + " END), [])";
  }
  if (type->IsStruct()) {
    const auto* structure = type->AsStruct();
    const std::vector<std::string> names = DuckDbStructFieldNames(structure);
    std::vector<std::string> fields;
    for (int i = 0; i < structure->num_fields(); ++i) {
      const auto rounded =
          RoundedValue("struct_extract_at(" + name + ", " + std::to_string(i + 1) + ")",
                       structure->field(i).type, field.fields.at(i), column, depth + 1);
      if (!rounded) {
        return std::nullopt;
      }
      fields.push_back(QuoteIdentifier(names.at(i)) + " := " + *rounded);
    }
    return "list_transform([" + value + "], " + name + " -> CASE WHEN " + name +
           " IS NULL THEN NULL ELSE struct_pack(" + Join(fields, ", ") + ") END)[1]";
  }
  auto rounded = TranslateFunction(
      "ROUND",
      {
          {.sql = value, .type = type->kind()},
          {.sql = std::to_string(field.scale.value_or(0)), .type = googlesql::TYPE_INT64},
          {.sql = "NULL", .type = googlesql::TYPE_ENUM, .rounding_mode = "ROUND_HALF_EVEN"},
      });
  if (!rounded || !type->IsNumericType() || !field.precision.has_value()) {
    return rounded;
  }
  // Check after rounding, before DuckDB's column cast, to report BigQuery's range error.
  const auto precision = *field.precision;
  const auto scale = field.scale.value_or(0);
  std::string bound = precision == scale ? "0" : std::string(precision - scale, '9');
  if (scale > 0) {
    bound += "." + std::string(scale, '9');
  }
  const std::string message =
      "Field " + std::string(column) + ": NUMERIC(" + std::to_string(precision) + ", " +
      std::to_string(scale) + ") has precision " + std::to_string(precision) + " and scale " +
      std::to_string(scale) + " but got a value that is not in range of [-" + bound + ", " + bound +
      "]";
  return "list_transform([" + *rounded + "], " + name + " -> CASE WHEN abs(" + name + ") > CAST(" +
         QuoteLiteral(bound) + " AS DECIMAL(38,9)) THEN error(" + QuoteLiteral(message) +
         ") ELSE " + name + " END)[1]";
}

}  // namespace

std::optional<std::string> WrittenValue(const std::string& value, const googlesql::Type* type,
                                        const FieldSchema& field) {
  return RoundedToEven(GeographyWkt(value, type), type, field);
}

std::optional<std::string> RoundedToEven(const std::string& value, const googlesql::Type* type,
                                         const FieldSchema& field) {
  return RoundedValue(value, type, field, field.name, 0);
}

namespace {

// What a function does with values of a type that DuckDB holds in another type, such as a RANGE
// in a STRUCT. A type takes the categories whose operations DuckDB performs on its stand-in as
// BigQuery does on its values; other functions would see the stand-in.
//
// These pass, select or count their values without reading them.
constexpr std::string_view kCarries[] = {
    "$IS_NULL",
    "$CASE_NO_VALUE",
    "IF",
    "IFNULL",
    "COALESCE",
    "$MAKE_ARRAY",
    "$ARRAY_AT_OFFSET",
    "$ARRAY_AT_ORDINAL",
    "$SAFE_ARRAY_AT_OFFSET",
    "$SAFE_ARRAY_AT_ORDINAL",
    "ARRAY_LENGTH",
    "ARRAY_REVERSE",
    "ARRAY_CONCAT",
};
// These also compare or order their values.
constexpr std::string_view kCompares[] = {
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
};

bool In(std::span<const std::string_view> functions, std::string_view function) {
  return std::ranges::find(functions, function) != functions.end();
}

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
  if (!IsBuiltinFunction(*call.function())) {
    return Unsupported(scope, "function " + name);
  }
  // BigQuery rejects this prefix on these functions although GoogleSQL accepts it.
  if ((name == "SESSION_USER" || name == "ERROR" || name == "TEXT_ANALYZE" ||
       name == "ST_GEOHASH") &&
      call.error_mode() == googlesql::ResolvedFunctionCallBase::SAFE_ERROR_MODE) {
    throw std::invalid_argument("SAFE with function " + call.function()->Name() +
                                " is not supported.");
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
  // INTERVAL values are DuckDB intervals, which these build and read.
  static constexpr std::string_view interval_functions[] = {
      "$INTERVAL",
      "$EXTRACT",
      "MAKE_INTERVAL",
      "JUSTIFY_HOURS",
      "JUSTIFY_DAYS",
      "JUSTIFY_INTERVAL",
      "ERROR",
      "FORMAT",
      "JSON_ARRAY_APPEND",
      "JSON_ARRAY_INSERT",
      "TO_JSON",
      "TO_JSON_STRING",
      "GENERATE_RANGE_ARRAY",
  };
  // BigQuery rejects these over INTERVAL values although GoogleSQL accepts them.
  if ((function == "GREATEST" || function == "LEAST" || function == "NULLIF") &&
      std::ranges::any_of(call.argument_list(),
                          [](const auto& argument) { return argument->type()->IsInterval(); })) {
    throw std::invalid_argument("Unsupported type in function " +
                                (function == "NULLIF" ? "NULL_IF" : function));
  }
  // These only compare their INTERVAL arguments or ARRAY elements, so they compare the
  // values' keys instead.
  static constexpr std::string_view interval_comparisons[] = {
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
      "RANGE_BUCKET",
  };
  const auto keyed = [&](const googlesql::Type* type) {
    return type->IsInterval() || ((function == "$IN_ARRAY" || function == "RANGE_BUCKET") &&
                                  type->IsArray() && type->AsArray()->element_type()->IsInterval());
  };
  const bool compares_intervals =
      interval &&
      std::ranges::find(interval_comparisons, function) != std::end(interval_comparisons) &&
      std::ranges::all_of(call.argument_list(), [&](const auto& argument) {
        return !HasInterval(argument->type()) || keyed(argument->type());
      });
  // StructComparison compares STRUCTs field by field, and their INTERVAL fields by their keys.
  // BigQuery compares no ARRAYs, also in STRUCT fields.
  static constexpr std::string_view struct_comparisons[] = {
      "$EQUAL",
      "$NOT_EQUAL",
      "$IN",
      "$IN_ARRAY",
  };
  const bool compares_structs =
      interval && In(struct_comparisons, function) && call.argument_list(0)->type()->IsStruct();
  if (interval && !compares_intervals && !compares_structs && entry->handler != Bucket &&
      !In(kCarries, function) && !In(interval_functions, function)) {
    return Unsupported(scope, "function " + name + " with INTERVAL");
  }
  // A GEOGRAPHY is text in DuckDB, which the S2 functions build and read, and which DuckDB
  // carries as BigQuery does; others, such as TO_JSON, would see a STRING.
  if ((HasGeography(call.type()) ||
       std::ranges::any_of(call.argument_list(),
                           [](const auto& argument) { return HasGeography(argument->type()); })) &&
      entry->implementation != Implementation::kS2 && !In(kCarries, function)) {
    return Unsupported(scope, "function " + name + " with GEOGRAPHY");
  }
  // BigQuery rejects RANGE_BUCKET over RANGE values although GoogleSQL accepts it.
  if (function == "RANGE_BUCKET" && call.argument_list(0)->type()->IsRange()) {
    throw std::invalid_argument("Unsupported type RANGE for function RANGE_BUCKET");
  }
  // A RANGE is a STRUCT in DuckDB whose unbounded ends are infinite, so DuckDB carries, compares
  // and orders it as BigQuery does a RANGE. These build or read it, and the JSON and FORMAT
  // backends read it as a RANGE; others would see the STRUCT.
  static constexpr std::string_view range_functions[] = {
      "FORMAT",
      "TO_JSON",
      "TO_JSON_STRING",
      "JSON_ARRAY",
      "JSON_OBJECT",
      "JSON_SET",
      "JSON_ARRAY_APPEND",
      "JSON_ARRAY_INSERT",
      "RANGE",
      "RANGE_START",
      "RANGE_END",
      "RANGE_CONTAINS",
      "RANGE_OVERLAPS",
      "RANGE_INTERSECT",
      "GENERATE_RANGE_ARRAY",
      "ERROR",
  };
  if ((HasRange(call.type()) ||
       std::ranges::any_of(call.argument_list(),
                           [](const auto& argument) { return HasRange(argument->type()); })) &&
      !In(kCarries, function) && !In(kCompares, function) && !In(range_functions, function)) {
    return Unsupported(scope, "function " + name + " with RANGE");
  }
  // A STRUCT<> is a STRUCT with a placeholder field in DuckDB, which the JSON and FORMAT backends
  // read as an empty STRUCT; others would see the field.
  static constexpr std::string_view empty_struct_functions[] = {
      "TO_JSON", "TO_JSON_STRING",    "JSON_ARRAY",        "JSON_OBJECT", "JSON_SET",
      "FORMAT",  "JSON_ARRAY_APPEND", "JSON_ARRAY_INSERT", "$MAKE_ARRAY",
  };
  if ((HasEmptyStruct(call.type()) || std::ranges::any_of(call.argument_list(),
                                                          [](const auto& argument) {
                                                            return HasEmptyStruct(argument->type());
                                                          })) &&
      std::ranges::find(empty_struct_functions, function) == std::end(empty_struct_functions)) {
    return Unsupported(scope, "function " + name + " with STRUCT<>");
  }
  // Call gives equality of STRUCTs to StructComparison, and BigQuery does not compare ARRAYs.
  // DuckDB would compare the STRUCTs of IS DISTINCT FROM by their internal names, with NULL fields
  // taken as values.
  if (function == "$IS_DISTINCT_FROM" && HasInternalStructNames(call.argument_list(0)->type())) {
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
    if (compares_intervals && argument->type()->IsInterval()) {
      args.push_back(IntervalKey(*sql));
    } else if (compares_intervals && keyed(argument->type())) {
      args.push_back("list_transform(" + *sql + ", _ik -> " + IntervalKey("_ik") + ")");
    } else {
      args.push_back(*sql);
    }
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
