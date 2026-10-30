#include "googlesql/public/function.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "googlesql/public/type.h"
#include "googlesql/public/value.h"
#include "googlesql/resolved_ast/resolved_ast.h"
#include "src/duckdb_sql.h"
#include "src/field_schema.h"
#include "src/function_catalog/catalog.h"
#include "src/function_catalog/comparison.h"
#include "src/function_catalog/resolved_access.h"
#include "src/function_catalog/rules.h"
#include "src/javascript_function.h"
#include "src/language_options.h"
#include "src/python_function.h"
#include "src/schema_sql.h"
#include "src/timestamp_precision.h"
#include "src/translator/collation.h"
#include "src/translator/context.h"
#include "src/translator/expression.h"
#include "src/translator/function.h"
#include "src/type_mapping.h"

namespace bigquery_emulator_duckdb::translator {

using function_catalog::ArgumentMode;
using function_catalog::BucketWidthOf;
using function_catalog::Catalog;
using function_catalog::DatePart;
using function_catalog::FunctionEntry;
using function_catalog::NormalizeMode;
using function_catalog::RejectArgumentTypes;
using function_catalog::RoundingModeOf;
using function_catalog::ScalarCall;
using function_catalog::Translation;
using function_catalog::TypeUse;

namespace {

// The DuckDB spelling of a scalar call over already translated arguments, whose TIMESTAMPs have
// `precision`.
std::optional<std::string> LegacyCall(const Catalog& functions,
                                      const googlesql::ResolvedFunctionCall& resolved,
                                      std::string_view name, const std::vector<std::string>& args,
                                      bool safe, std::string_view project,
                                      const Collation& collation, TimestampPrecision precision) {
  const FunctionEntry* entry = functions.Find(name);
  if (entry == nullptr) {
    return std::nullopt;
  }
  if (name == "NULLIF" && !collation.empty()) {
    // Compare keys, but return the original first argument, evaluated only once.
    const std::string first = function_catalog::BoundComparison(0);
    const std::string second = function_catalog::BoundComparison(1);
    return function_catalog::BindComparison(
        args, {0, 1},
        "CASE WHEN " +
            function_catalog::Equal(
                resolved.argument_list(0)->type(),
                CollationKey(first, collation, resolved.argument_list(0)->type()),
                CollationKey(second, collation, resolved.argument_list(1)->type())) +
            " THEN NULL ELSE " + first + " END");
  }
  if ((name == "GREATEST" || name == "LEAST") && !collation.empty()) {
    // NULL when any argument is, and otherwise the first argument whose key is the extremum.
    return "list_transform([[" + Join(args, ", ") +
           "]], lambda _ext: CASE WHEN list_count(_ext) < len(_ext) THEN NULL ELSE "
           "list_transform([list_transform(_ext, lambda _e: " +
           CollationKey("_e", collation, resolved.argument_list(0)->type()) +
           ")], lambda _keys: _ext[list_position(_keys, " +
           (name == "GREATEST" ? "list_max" : "list_min") + "(_keys))])[1] END)[1]";
  }
  ScalarCall call{
      .resolved = resolved,
      .name = name,
      .safe = safe,
      .project = project,
      .sql_timestamp_precision = precision,
  };
  for (size_t i = 0; i < args.size(); ++i) {
    const googlesql::ResolvedExpr& argument = *resolved.argument_list(static_cast<int>(i));
    call.arguments.push_back({
        .sql = args.at(i),
        .type = argument.type()->kind(),
        .date_part = DatePart(argument),
        .rounding_mode = RoundingModeOf(argument),
    });
  }
  if (!args.empty() && resolved.argument_list(0)->type()->IsStruct() &&
      entry->struct_handler != nullptr) {
    return entry->struct_handler(call);
  }
  if (entry->translation == Translation::kHandler) {
    return entry->handler(call);
  }
  return functions.Translate(name, call.arguments, safe,
                             functions.SupportsCollation(name) ? collation.specification : "");
}

// Functions that only carry or compare values can operate directly on epoch nanoseconds.
// Other existing overloads may use the microsecond backend only when the conversion is exact.
// The adapter rejects finer inputs instead of silently truncating them.
std::optional<std::string> Call(const Catalog& functions,
                                const googlesql::ResolvedFunctionCall& resolved,
                                std::string_view name, const std::vector<std::string>& args,
                                bool safe, std::string_view project, const Collation& collation) {
  if (functions.timestamp_precision() != TimestampPrecision::kNanoseconds ||
      functions.HasTypeUse(name, TypeUse::kCarries) ||
      functions.HasTypeUse(name, TypeUse::kCompares) ||
      functions.HasTypeUse(name, TypeUse::kCarriesResults)) {
    return LegacyCall(functions, resolved, name, args, safe, project, collation,
                      functions.timestamp_precision());
  }
  std::vector<std::string> converted = args;
  for (size_t i = 0; i < args.size(); ++i) {
    const auto* type = resolved.argument_list(static_cast<int>(i))->type();
    if (type->IsTimestamp()) {
      converted.at(i) =
          "bq_timestamp_nanos_to_micros(CAST(" + args.at(i) + " AS bq_timestamp_nanos))";
    } else if (HasTimestamp(type, /*range_bounds=*/false)) {
      return std::nullopt;
    }
  }
  if (HasTimestamp(resolved.type(), /*range_bounds=*/false) && !resolved.type()->IsTimestamp()) {
    return std::nullopt;
  }
  auto sql = LegacyCall(functions, resolved, name, converted, safe, project, collation,
                        TimestampPrecision::kMicroseconds);
  if (sql && resolved.type()->IsTimestamp()) {
    *sql = "bq_timestamp_nanos_from_micros(" + *sql + ")";
  }
  return sql;
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

// Whether ResultValue changes values of `type`.
bool HasResultForm(const googlesql::Type* type, bool stored) {
  if (type->IsArray()) {
    return true;
  }
  if (type->IsStruct()) {
    return std::ranges::any_of(type->AsStruct()->fields(),
                               [stored](const googlesql::StructField& field) {
                                 return HasResultForm(field.type, stored);
                               });
  }
  return !stored && type->IsGeography();
}

// What a NULL element of an ARRAY written to `column` becomes: an error under BigQuery's
// restriction.
std::string NullElement(const Catalog& functions, std::string_view column) {
  return functions.restrictions().arrays_without_null_elements
             ? "error(" +
                   QuoteLiteral("Array cannot have a null element; error in writing field " +
                                std::string(column)) +
                   ")"
             : "NULL";
}

std::string ResultValue(const Catalog& functions, const std::string& value,
                        const googlesql::Type* type, std::string_view column, bool stored,
                        int depth) {
  const TimestampPrecision precision = functions.timestamp_precision();
  if (precision == TimestampPrecision::kNanoseconds && type->IsTimestamp()) {
    return "CAST(" + value + " AS bq_timestamp_nanos)";
  }
  if (!HasResultForm(type, stored) &&
      (precision != TimestampPrecision::kNanoseconds || !HasTimestamp(type, false))) {
    return value;
  }
  if (type->IsGeography()) {
    return "bq_st_astext(" + value + ")";
  }
  // Each level binds its value once, under a name of its own.
  const std::string name = "_result" + std::to_string(depth);
  if (type->IsArray()) {
    return "list_transform(" + value + ", lambda " + name + ": CASE WHEN " + name +
           " IS NULL THEN " + NullElement(functions, column) + " ELSE " +
           ResultValue(functions, name, type->AsArray()->element_type(), column, stored,
                       depth + 1) +
           " END)";
  }
  const auto* structure = type->AsStruct();
  const std::vector<std::string> names = DuckDbStructFieldNames(structure);
  std::vector<std::string> fields;
  fields.reserve(names.size());
  for (int i = 0; i < structure->num_fields(); ++i) {
    fields.push_back(QuoteIdentifier(names.at(i)) + " := " +
                     ResultValue(functions,
                                 "struct_extract_at(" + name + ", " + std::to_string(i + 1) + ")",
                                 structure->field(i).type, column, stored, depth + 1));
  }
  return "list_transform([" + value + "], lambda " + name + ": CASE WHEN " + name +
         " IS NULL THEN NULL ELSE struct_pack(" + Join(fields, ", ") + ") END)[1]";
}

}  // namespace

std::string ResultValue(const Catalog& functions, const std::string& value,
                        const googlesql::Type* type, std::string_view column, bool stored) {
  return ResultValue(functions, value, type, column, stored, 0);
}

namespace {

// Bind each container once: rebuilding a STRUCT must not evaluate its expression per field.
std::optional<std::string> RoundedValue(const Catalog& functions, const std::string& value,
                                        const googlesql::Type* type, const FieldSchema& field,
                                        std::string_view column, int depth) {
  if (!RoundsToEven(field)) {
    return value;
  }
  const std::string name = "_round" + std::to_string(depth);
  if (type->IsArray()) {
    const auto element =
        RoundedValue(functions, name, type->AsArray()->element_type(), field, column, depth + 1);
    if (!element) {
      return std::nullopt;
    }
    return "coalesce(list_transform(" + value + ", lambda " + name + ": CASE WHEN " + name +
           " IS NULL THEN " + NullElement(functions, column) + " ELSE " + *element + " END), [])";
  }
  if (type->IsStruct()) {
    const auto* structure = type->AsStruct();
    const std::vector<std::string> names = DuckDbStructFieldNames(structure);
    std::vector<std::string> fields;
    for (int i = 0; i < structure->num_fields(); ++i) {
      const auto rounded =
          RoundedValue(functions, "struct_extract_at(" + name + ", " + std::to_string(i + 1) + ")",
                       structure->field(i).type, field.fields.at(i), column, depth + 1);
      if (!rounded) {
        return std::nullopt;
      }
      fields.push_back(QuoteIdentifier(names.at(i)) + " := " + *rounded);
    }
    return "list_transform([" + value + "], lambda " + name + ": CASE WHEN " + name +
           " IS NULL THEN NULL ELSE struct_pack(" + Join(fields, ", ") + ") END)[1]";
  }
  auto rounded = functions.Translate(
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
  return "list_transform([" + *rounded + "], lambda " + name + ": CASE WHEN abs(" + name +
         ") > CAST(" + QuoteLiteral(bound) + " AS DECIMAL(38,9)) THEN error(" +
         QuoteLiteral(message) + ") ELSE " + name + " END)[1]";
}

}  // namespace

std::optional<std::string> WrittenValue(const Catalog& functions, const std::string& value,
                                        const googlesql::Type* type, const FieldSchema& field,
                                        bool assignment) {
  const auto rounded =
      RoundedToEven(functions, ResultValue(functions, value, type, field.name, true), type, field);
  return rounded ? std::optional<std::string>(CheckedWriteValue(*rounded, field, assignment))
                 : std::nullopt;
}

std::optional<std::string> RoundedToEven(const Catalog& functions, const std::string& value,
                                         const googlesql::Type* type, const FieldSchema& field) {
  return RoundedValue(functions, value, type, field, field.name, 0);
}

namespace {

// An external-language UDF, run by the backend function for its result type.
std::optional<std::string> ExternalCall(const googlesql::ResolvedFunctionCall& call,
                                        const std::string& source, const Scope& scope,
                                        const Columns& columns, bool python = false) {
  const std::string name = call.function()->Name();
  if (call.error_mode() != googlesql::ResolvedFunctionCallBase::DEFAULT_ERROR_MODE) {
    return Unsupported(scope,
                       python ? "SAFE. calls of Python UDFs" : "SAFE. calls of JavaScript UDFs");
  }
  const googlesql::Type* type = call.type();
  const char* result = type->IsBool()              ? "bool"
                       : type->IsDouble()          ? "double"
                       : type->IsString()          ? "string"
                       : type->IsInt64()           ? "int64"
                       : python && type->IsBytes() ? "bytes"
                                                   : nullptr;
  if (result == nullptr) {
    return Unsupported(
        scope, std::string(python ? "Python" : "JavaScript") + " UDF " + name + " result type");
  }
  std::vector<std::string> args = {QuoteLiteral(source)};
  for (const auto& argument : call.argument_list()) {
    const auto sql = Expression(*argument, scope, columns);
    if (scope.context.timestamp_precision == TimestampPrecision::kNanoseconds &&
        HasTimestamp(argument->type(), false)) {
      return Unsupported(scope, "nanosecond TIMESTAMP argument of an external-language UDF");
    }
    const auto duckdb_type = DuckDbType(argument->type(), scope.context.timestamp_precision);
    if (!sql || !duckdb_type) {
      return std::nullopt;
    }
    args.push_back("CAST(" + *sql + " AS " + *duckdb_type + ")");
  }
  return std::string(python ? "bq_py_" : "bq_js_") + result + "(" + Join(args, ", ") + ")";
}

}  // namespace

std::optional<std::string> Function(const googlesql::ResolvedFunctionCall& call, const Scope& scope,
                                    const Columns& columns) {
  const Catalog& functions = scope.context.functions;
  const std::string name = ToUpperAscii(call.function()->FullName(/*include_group=*/false));
  if (!call.generic_argument_list().empty() || !call.hint_list().empty()) {
    return Unsupported(scope, "function " + name + " with generic arguments or hints");
  }
  const auto collation = OperationCollation(call.collation_list(), scope);
  if (!collation) {
    return std::nullopt;
  }
  if (!collation->empty() && functions.HasTypeUse(name, TypeUse::kRejectsCollation)) {
    throw std::invalid_argument("Function '" + call.function()->Name() + "' with collation '" +
                                collation->specification + "' is not supported.");
  }
  if (!collation->children.empty() &&
      !functions.HasTypeUse(name, TypeUse::kComparesCollationKeys) && name != "NULLIF" &&
      name != "$CASE_WITH_VALUE") {
    return Unsupported(scope, "function " + name + " with nested collation");
  }
  if (!collation->empty() && !functions.HasTypeUse(name, TypeUse::kComparesCollationKeys) &&
      !functions.HasTypeUse(name, TypeUse::kSelectsCollationValues) &&
      !functions.SupportsCollation(name)) {
    return Unsupported(scope, "function " + name + " with collation");
  }
  if (const auto* python = dynamic_cast<const PythonFunction*>(call.function())) {
    return ExternalCall(call, python->definition(), scope, columns, true);
  }
  if (const auto* javascript = dynamic_cast<const JavaScriptFunction*>(call.function())) {
    return ExternalCall(call, javascript->source(), scope, columns);
  }
  if (!IsBuiltinFunction(*call.function())) {
    return Unsupported(scope, "function " + name);
  }
  // BigQuery rejects this prefix on these functions although GoogleSQL accepts it.
  if (functions.HasTypeUse(name, TypeUse::kRejectsSafe) &&
      call.error_mode() == googlesql::ResolvedFunctionCallBase::SAFE_ERROR_MODE) {
    throw std::invalid_argument("SAFE with function " +
                                call.function()->FullName(/*include_group=*/false) +
                                " is not supported.");
  }
  if (name == "SESSION_USER" && !scope.context.defaults.has_session_user) {
    return Unsupported(scope, "function SESSION_USER");
  }
  const FunctionEntry* entry = functions.Find(name);
  if (entry == nullptr) {
    return Unsupported(scope, "function " + name);
  }
  const bool alias = entry->translation == Translation::kSafe;
  const std::string function = alias ? std::string(entry->target) : name;
  if (alias) {
    entry = functions.Find(function);
    if (entry == nullptr) {
      return Unsupported(scope, "function " + name);
    }
  }
  const auto argument_mode = [&](size_t i) {
    return i < entry->argument_modes.size() ? entry->argument_modes.at(i)
                                            : ArgumentMode::kExpression;
  };
  const bool safe =
      call.error_mode() == googlesql::ResolvedFunctionCallBase::SAFE_ERROR_MODE || alias;
  if (!safe && call.error_mode() != googlesql::ResolvedFunctionCallBase::DEFAULT_ERROR_MODE) {
    return Unsupported(scope, "function " + name + " error mode");
  }
  if (!functions.HasTypeUse(function, TypeUse::kBigNumeric) && InvolvesBigNumeric(call)) {
    return Unsupported(scope, "function " + name + " with BIGNUMERIC");
  }
  // Private graph metadata is not a SQL STRUCT's value. Opt functions in explicitly rather
  // than accidentally comparing properties, formatting metadata or serializing private IDs.
  if ((HasGraph(call.type()) ||
       std::ranges::any_of(call.argument_list(),
                           [](const auto& argument) { return HasGraph(argument->type()); })) &&
      !functions.HasTypeUse(function, TypeUse::kGraphValues)) {
    return Unsupported(scope, "function " + name + " with graph values");
  }
  const bool interval = HasInterval(call.type()) ||
                        std::ranges::any_of(call.argument_list(), [](const auto& argument) {
                          return HasInterval(argument->type());
                        });
  RejectArgumentTypes(call, entry->type_rejections);
  const auto keyed = [&](const googlesql::Type* type, size_t i) {
    return type->IsInterval() || (argument_mode(i) == ArgumentMode::kIntervalArrayKeys &&
                                  type->IsArray() && type->AsArray()->element_type()->IsInterval());
  };
  bool compares_intervals =
      interval && functions.HasTypeUse(function, TypeUse::kComparesIntervalKeys);
  for (size_t i = 0; i < call.argument_list().size(); ++i) {
    const auto* type = call.argument_list(static_cast<int>(i))->type();
    if (HasInterval(type) && !keyed(type, i)) {
      compares_intervals = false;
    }
  }
  const bool compares_structs =
      interval && functions.HasTypeUse(function, TypeUse::kComparesStructsWithInterval) &&
      call.argument_list(0)->type()->IsStruct();
  // CASE with a value carries its results when it compares no INTERVAL values.
  const bool carries_results = functions.HasTypeUse(function, TypeUse::kCarriesResults) &&
                               !HasInterval(call.argument_list(0)->type());
  // A declared conversion permits INTERVAL only in the argument it converts, not in the
  // result or nested inside another argument's ARRAY or STRUCT.
  bool converts_intervals = !HasInterval(call.type());
  for (size_t i = 0; i < call.argument_list().size(); ++i) {
    const auto* type = call.argument_list(static_cast<int>(i))->type();
    if (HasInterval(type) &&
        (!type->IsInterval() || argument_mode(i) != ArgumentMode::kBucketWidth)) {
      converts_intervals = false;
    }
  }
  // Enabling a physical type must not silently enable arithmetic, comparisons or JSON conversion.
  if (interval && !compares_intervals && !compares_structs && !carries_results &&
      !converts_intervals && !functions.HasTypeUse(function, TypeUse::kCarries) &&
      !functions.HasTypeUse(function, TypeUse::kBuildsOrReadsInterval)) {
    return Unsupported(scope, "function " + name + " with INTERVAL");
  }
  // A GEOGRAPHY is a BLOB in DuckDB, which the S2 functions build and read, and which DuckDB
  // carries as BigQuery does; others, such as TO_JSON, would see a BYTES.
  if ((HasGeography(call.type()) ||
       std::ranges::any_of(call.argument_list(),
                           [](const auto& argument) { return HasGeography(argument->type()); })) &&
      !functions.HasTypeUse(function, TypeUse::kBuildsOrReadsGeography) &&
      !functions.HasTypeUse(function, TypeUse::kCarries)) {
    return Unsupported(scope, "function " + name + " with GEOGRAPHY");
  }
  // A RANGE is a STRUCT in DuckDB, which only these read as a RANGE.
  if ((HasRange(call.type()) ||
       std::ranges::any_of(call.argument_list(),
                           [](const auto& argument) { return HasRange(argument->type()); })) &&
      !functions.HasTypeUse(function, TypeUse::kCarries) &&
      !functions.HasTypeUse(function, TypeUse::kCompares) &&
      !functions.HasTypeUse(function, TypeUse::kBuildsOrReadsRange)) {
    return Unsupported(scope, "function " + name + " with RANGE");
  }
  // A STRUCT<> is a STRUCT with a placeholder field in DuckDB, which only these read as empty.
  if ((HasEmptyStruct(call.type()) || std::ranges::any_of(call.argument_list(),
                                                          [](const auto& argument) {
                                                            return HasEmptyStruct(argument->type());
                                                          })) &&
      !functions.HasTypeUse(function, TypeUse::kReadsEmptyStruct)) {
    return Unsupported(scope, "function " + name + " with STRUCT<>");
  }
  std::vector<std::string> args;
  for (size_t i = 0; i < call.argument_list().size(); ++i) {
    const auto& argument = call.argument_list(static_cast<int>(i));
    // The bucket width INTERVAL becomes a plain count, since DuckDB intervals keep no single
    // part to count in.
    if (argument_mode(i) == ArgumentMode::kBucketWidth) {
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
      if (const auto mode = RoundingModeOf(*argument)) {
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
    // CASE compares only its input and WHEN operands; its results retain their values. NULLIF,
    // GREATEST and LEAST return values they compare, so Call binds their values before keying.
    const bool collation_operand =
        function == "$CASE_WITH_VALUE"
            ? i == 0 || (i % 2 == 1 && i + 1 < call.argument_list().size())
            : functions.HasTypeUse(function, TypeUse::kComparesCollationKeys);
    if (collation_operand && !collation->empty()) {
      args.push_back(CollationKey(*sql, *collation, argument->type()));
    } else if (compares_intervals && argument->type()->IsInterval()) {
      args.push_back(IntervalKey(*sql));
    } else if (compares_intervals && keyed(argument->type(), i)) {
      args.push_back("list_transform(" + *sql + ", lambda _ik: " + IntervalKey("_ik") + ")");
    } else {
      args.push_back(*sql);
    }
  }
  if (!safe) {
    auto sql =
        Call(functions, call, function, args, false, scope.context.defaults.project, *collation);
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
  const auto sql = Call(functions, call, function, placeholders, true,
                        scope.context.defaults.project, *collation);
  if (!sql) {
    return Unsupported(scope, "SAFE." + name);
  }
  if (bound.empty()) {
    return "TRY(" + *sql + ")";
  }
  return "list_transform([struct_pack(" + Join(bound, ", ") + ")], lambda " + lambda + ": TRY(" +
         *sql + "))[1]";
}

}  // namespace bigquery_emulator_duckdb::translator
