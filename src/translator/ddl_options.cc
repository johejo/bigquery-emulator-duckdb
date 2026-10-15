#include "src/translator/ddl_options.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "absl/time/time.h"
#include "googlesql/public/type.h"
#include "googlesql/public/value.h"
#include "googlesql/resolved_ast/resolved_ast.h"
#include "src/duckdb_sql.h"
#include "src/field_schema.h"
#include "src/table_metadata.h"
#include "src/translated_statement.h"
#include "src/translator/context.h"
#include "src/translator/expression.h"

namespace bigquery_emulator_duckdb::translator {
namespace {

// TIMESTAMP options use the same millisecond precision as Table.expirationTime.
bool ExpirationOption(const googlesql::Value& value, std::optional<int64_t>& into) {
  if (value.is_null()) {
    into.reset();
    return true;
  }
  if (!value.type()->IsTimestamp()) {
    return false;
  }
  into = absl::ToUnixMillis(value.ToUnixPicos().ToAbslTime());
  return true;
}

// The default_table_expiration_days option, a FLOAT64 number of days, in the milliseconds of
// Dataset.defaultTableExpirationMs; 0, as there, and NULL clear it.
bool DaysOption(const googlesql::Value& value, std::optional<int64_t>& into) {
  if (value.is_null()) {
    into.reset();
    return true;
  }
  if (!value.type()->IsDouble() && !value.type()->IsInt64()) {
    return false;
  }
  constexpr double kMillisecondsPerDay = 86400000;
  const double milliseconds = std::round(
      (value.type()->IsDouble() ? value.double_value() : static_cast<double>(value.int64_value())) *
      kMillisecondsPerDay);
  // Days whose milliseconds do not fit in an INT64 are rejected.
  if (!std::isfinite(milliseconds) || std::abs(milliseconds) >= 0x1p63) {
    return false;
  }
  into = milliseconds == 0 ? std::nullopt : std::optional(static_cast<int64_t>(milliseconds));
  return true;
}

// The labels option, an ARRAY<STRUCT<STRING, STRING>> of keys and values, into `into`.
bool LabelsOption(const googlesql::Value& value, std::map<std::string, std::string>& into) {
  into.clear();
  if (value.is_null()) {
    return true;
  }
  const googlesql::Type* type = value.type();
  if (!type->IsArray() || !type->AsArray()->element_type()->IsStruct()) {
    return false;
  }
  const googlesql::StructType* entry = type->AsArray()->element_type()->AsStruct();
  if (entry->num_fields() != 2 || !entry->field(0).type->IsString() ||
      !entry->field(1).type->IsString()) {
    return false;
  }
  for (int i = 0; i < value.num_elements(); ++i) {
    const googlesql::Value& element = value.element(i);
    if (element.is_null() || element.field(0).is_null() || element.field(1).is_null()) {
      return false;
    }
    into[element.field(0).string_value()] = element.field(1).string_value();
  }
  return true;
}

// The scalar functions that BigQuery's DDL reference excludes from option values.
constexpr std::array<std::string_view, 11> kExcludedOptionFunctions = {
    "ARRAY_TO_STRING",
    "REPLACE",
    "REGEXP_REPLACE",
    "RAND",
    "FORMAT",
    "LPAD",
    "RPAD",
    "REPEAT",
    "SESSION_USER",
    "GENERATE_ARRAY",
    "GENERATE_DATE_ARRAY",
};

// Whether `expression`, the value of `option` of `object` such as a table, is what BigQuery's DDL
// reference allows as an option value: "a constant expression containing only literals, query
// parameters, and scalar functions", other than the functions it excludes, which fail. Anything
// else, such as a script variable in parentheses, is unsupported.
bool ConstantOptionValue(const googlesql::ResolvedExpr& expression, std::string_view option,
                         std::string_view object, const Scope& scope) {
  const auto constant = [&](const googlesql::ResolvedExpr& element) {
    return ConstantOptionValue(element, option, object, scope);
  };
  const auto all = [&](const auto& expressions) {
    return std::ranges::all_of(expressions,
                               [&](const auto& element) { return constant(*element); });
  };
  if (expression.Is<googlesql::ResolvedLiteral>() ||
      expression.Is<googlesql::ResolvedParameter>()) {
    return true;
  }
  if (expression.Is<googlesql::ResolvedFunctionCall>()) {
    const auto* call = expression.GetAs<googlesql::ResolvedFunctionCall>();
    const std::string name = ToUpperAscii(call->function()->Name());
    if (std::ranges::find(kExcludedOptionFunctions, name) != kExcludedOptionFunctions.end()) {
      throw std::invalid_argument("Found unsupported function call '" + name +
                                  "'; failed to evaluate '" + std::string(option) + "' in " +
                                  std::string(object) + " OPTIONS");
    }
    if (!call->function()->IsGoogleSQLBuiltin() && name != "CONTAINS_SUBSTR") {
      Unsupported(scope, "function " + name + " in option values");
      return false;
    }
    return all(call->argument_list());
  }
  if (expression.Is<googlesql::ResolvedCast>()) {
    const auto* cast = expression.GetAs<googlesql::ResolvedCast>();
    return constant(*cast->expr()) && (cast->format() == nullptr || constant(*cast->format())) &&
           (cast->time_zone() == nullptr || constant(*cast->time_zone()));
  }
  if (expression.Is<googlesql::ResolvedMakeStruct>()) {
    const auto* make = expression.GetAs<googlesql::ResolvedMakeStruct>();
    return all(make->field_list());
  }
  Unsupported(scope, "option values other than literals, query parameters and scalar functions");
  return false;
}

// The object whose options `statement`, such as CREATE TABLE, sets, as BigQuery's errors name it.
std::string OptionObject(std::string_view statement) {
  return ToLowerAscii(statement.substr(statement.find(' ') + 1));
}

}  // namespace

// The value an option of `object`, such as a table, assigns: a literal, or a constant expression,
// which the backend evaluates as a query would. nullopt for += and -=, and for a value the
// emulator does not support, which `scope` may then name.
std::optional<googlesql::Value> OptionValue(const googlesql::ResolvedOption& option,
                                            std::string_view object, const Scope& scope) {
  if (!option.qualifier().empty() ||
      option.assignment_op() != googlesql::ResolvedOption::DEFAULT_ASSIGN) {
    return std::nullopt;
  }
  const googlesql::ResolvedExpr& expression = *option.value();
  if (expression.Is<googlesql::ResolvedLiteral>()) {
    return expression.GetAs<googlesql::ResolvedLiteral>()->value();
  }
  const auto& evaluate = scope.context.defaults.evaluate;
  if (!evaluate || !ConstantOptionValue(expression, option.name(), object, scope)) {
    return std::nullopt;
  }
  const std::optional<std::string> sql = Expression(expression, scope, {});
  if (!sql.has_value()) {
    return std::nullopt;
  }
  return evaluate("SELECT " + *sql, expression.type());
}

// A STRING option into `into`; NULL leaves it unset.
bool StringOption(const googlesql::Value& value, std::string& into) {
  if (value.is_null()) {
    into.clear();
    return true;
  }
  if (!value.type()->IsString()) {
    return false;
  }
  into = value.string_value();
  return true;
}

bool RoundingModeOption(const googlesql::Value& value, std::string& into) {
  std::string mode;
  if (!StringOption(value, mode) ||
      (!mode.empty() && mode != kRoundHalfAwayFromZero && mode != kRoundHalfEven)) {
    return false;
  }
  into = std::move(mode);
  return true;
}

// The metadata the OPTIONS of a CREATE TABLE or CREATE VIEW set; only a table has a default
// rounding mode.
std::optional<TableMetadata> OptionsMetadata(const Options& options, std::string_view statement,
                                             const Scope& scope, TableMetadata metadata) {
  for (const auto& option : options) {
    const std::string name = ToLowerAscii(option->name());
    const std::optional<googlesql::Value> value =
        OptionValue(*option, OptionObject(statement), scope);
    const bool applied =
        value.has_value() &&
        ((name == "description" && StringOption(*value, metadata.description)) ||
         (name == "friendly_name" && StringOption(*value, metadata.friendly_name)) ||
         (name == "labels" && LabelsOption(*value, metadata.labels)) ||
         (name == "expiration_timestamp" && ExpirationOption(*value, metadata.expiration_time)) ||
         (statement == "CREATE TABLE" && name == "default_rounding_mode" &&
          RoundingModeOption(*value, metadata.default_rounding_mode)));
    if (!applied) {
      return Unsupported(scope, std::string(statement) + " option " + option->name());
    }
  }
  return metadata;
}

// The description, friendly name, labels, default table expiration and default rounding mode the
// OPTIONS of a CREATE SCHEMA set. Every dataset is in the US, so a location elsewhere is
// unsupported, and so are the other options, which change how BigQuery treats the dataset's tables.
std::optional<DatasetMetadata> SchemaOptionsMetadata(const Options& options, const Scope& scope) {
  DatasetMetadata metadata;
  for (const auto& option : options) {
    const std::string name = ToLowerAscii(option->name());
    const std::optional<googlesql::Value> value = OptionValue(*option, "schema", scope);
    std::string location;
    const bool applied =
        value.has_value() &&
        ((name == "description" && StringOption(*value, metadata.description)) ||
         (name == "friendly_name" && StringOption(*value, metadata.friendly_name)) ||
         (name == "labels" && LabelsOption(*value, metadata.labels)) ||
         (name == "default_table_expiration_days" &&
          DaysOption(*value, metadata.default_table_expiration_ms)) ||
         (name == "default_rounding_mode" &&
          RoundingModeOption(*value, metadata.default_rounding_mode)) ||
         (name == "location" && StringOption(*value, location) &&
          (value->is_null() || ToUpperAscii(location) == "US")));
    if (!applied) {
      return Unsupported(scope, "CREATE SCHEMA option " + option->name());
    }
  }
  return metadata;
}

// The metadata SET OPTIONS of `statement` sets on `updates`. The
// other options change how BigQuery stores or reads the table or dataset, which the
// emulator does not emulate, so they are unsupported, and so are += and -=.
bool SetOptions(const Options& options, std::string_view statement, const Scope& scope,
                OptionUpdates& updates) {
  for (const auto& option : options) {
    const std::string name = ToLowerAscii(option->name());
    const std::optional<googlesql::Value> value =
        OptionValue(*option, OptionObject(statement), scope);
    const bool applied =
        value.has_value() &&
        ((name == "description" && StringOption(*value, updates.description.emplace())) ||
         (name == "friendly_name" && StringOption(*value, updates.friendly_name.emplace())) ||
         (name == "labels" && LabelsOption(*value, updates.labels.emplace())) ||
         (statement == "ALTER TABLE" && name == "expiration_timestamp" &&
          ExpirationOption(*value, updates.expiration_time.emplace())) ||
         (statement == "ALTER SCHEMA" && name == "default_table_expiration_days" &&
          DaysOption(*value, updates.default_table_expiration_ms.emplace())) ||
         ((statement == "ALTER TABLE" || statement == "ALTER SCHEMA") &&
          name == "default_rounding_mode" &&
          RoundingModeOption(*value, updates.default_rounding_mode.emplace())));
    if (!applied) {
      Unsupported(scope, std::string(statement) + " option " + option->name());
      return false;
    }
  }
  return true;
}

}  // namespace bigquery_emulator_duckdb::translator
