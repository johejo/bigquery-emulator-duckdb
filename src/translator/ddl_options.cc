#include "src/translator/ddl_options.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/time/time.h"
#include "googlesql/public/type.h"
#include "googlesql/public/value.h"
#include "googlesql/resolved_ast/resolved_ast.h"
#include "src/api_error.h"
#include "src/ddl_option_types.h"
#include "src/duckdb_sql.h"
#include "src/field_schema.h"
#include "src/language_options.h"
#include "src/location.h"
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

bool BoolOption(const googlesql::Value& value, bool& into) {
  if (!value.is_null() && !value.type()->IsBool()) {
    return false;
  }
  into = !value.is_null() && value.bool_value();
  return true;
}

// The labels option, an ARRAY<STRUCT<STRING, STRING>> of keys and values, into `into`.
bool LabelsOption(const googlesql::Value& value, std::map<std::string, std::string>& into,
                  std::vector<std::string>& order) {
  into.clear();
  order.clear();
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
    const std::string& key = element.field(0).string_value();
    if (!into.contains(key)) {
      order.push_back(key);
    }
    into[key] = element.field(1).string_value();
  }
  return true;
}

// Tags have the same pair representation as labels, but cannot repeat a key.
// NULL clears tags; incremental additions stay unsupported until their ordering is verified.
bool TagsOption(const googlesql::Value& value, std::map<std::string, std::string>& into,
                std::vector<std::string>& order) {
  if (!LabelsOption(value, into, order) ||
      (!value.is_null() && static_cast<size_t>(value.num_elements()) != into.size())) {
    return false;
  }
  ValidateResourceTags(into);
  return true;
}

// The scalar functions that BigQuery's DDL reference excludes from option values.
constexpr auto kExcludedOptionFunctions = std::to_array<std::string_view>({
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
});

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
    if (!IsBuiltinFunction(*call->function())) {
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
  if (statement.ends_with("FUNCTION")) {
    return "function";
  }
  return ToLowerAscii(statement.substr(statement.find(' ') + 1));
}

// A location as CanonicalLocation spells it; NULL leaves it unset.
bool LocationOption(const googlesql::Value& value, std::string& into) {
  std::string location;
  if (!StringOption(value, location)) {
    return false;
  }
  if (location.empty()) {
    into.clear();
    return true;
  }
  std::optional<std::string> canonical = CanonicalLocation(location);
  if (!canonical.has_value()) {
    throw ApiError::InvalidQuery("Invalid location '" + location + "'");
  }
  into = *std::move(canonical);
  return true;
}

}  // namespace

// The value an option of `object`, such as a table, assigns: a literal, or a constant expression,
// which the backend evaluates as a query would. nullopt for an unsupported assignment or value the
// emulator does not support, which `scope` may then name.
std::optional<googlesql::Value> OptionValue(const googlesql::ResolvedOption& option,
                                            DdlOptionContext context, const Scope& scope) {
  const auto context_name = DdlOptionContextName(context);
  const std::string object =
      context == DdlOptionContext::kColumn ? "column" : OptionObject(context_name);
  const auto* definition = FindDdlOption(context, ToLowerAscii(option.name()));
  if (definition == nullptr) {
    Unsupported(scope, std::string(context_name) + " option " + option.name());
    return std::nullopt;
  }
  const auto checked = [&](std::optional<googlesql::Value> value) {
    if (value && !DdlOptionValueMatches(definition->type, *value)) {
      return std::optional<googlesql::Value>{};
    }
    return value;
  };
  const bool removing_tags = context == DdlOptionContext::kAlterTable &&
                             definition->name == DdlOptionId::kTags &&
                             option.assignment_op() == googlesql::ResolvedOption::SUB_ASSIGN;
  if (!option.qualifier().empty() ||
      (option.assignment_op() != googlesql::ResolvedOption::DEFAULT_ASSIGN && !removing_tags)) {
    return std::nullopt;
  }
  const googlesql::ResolvedExpr& expression = *option.value();
  if (expression.Is<googlesql::ResolvedLiteral>()) {
    return checked(expression.GetAs<googlesql::ResolvedLiteral>()->value());
  }
  const auto& evaluate = scope.context.defaults.evaluate;
  if (!evaluate || !ConstantOptionValue(expression, option.name(), object, scope)) {
    return std::nullopt;
  }
  const std::optional<std::string> sql = Expression(expression, scope, {});
  if (!sql.has_value()) {
    return std::nullopt;
  }
  return checked(evaluate("SELECT " + *sql, expression.type()));
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

bool RoundingModeOption(const googlesql::Value& value, RoundingMode& into) {
  std::string mode;
  if (!StringOption(value, mode) || mode == "ROUNDING_MODE_UNSPECIFIED") {
    return false;
  }
  const auto parsed = ParseRoundingMode(mode);
  if (!parsed) {
    return false;
  }
  into = *parsed;
  return true;
}

// The metadata the OPTIONS of a CREATE TABLE or CREATE VIEW set; only a table has a default
// rounding mode.
std::optional<TableMetadata> OptionsMetadata(const Options& options, DdlOptionContext statement,
                                             const Scope& scope, TableMetadata metadata) {
  using enum DdlOptionId;
  for (const auto& option : options) {
    const auto name = ParseDdlOptionId(ToLowerAscii(option->name()));
    const auto value = OptionValue(*option, statement, scope);
    const bool table = statement == DdlOptionContext::kCreateTable;
    const bool applied =
        value &&
        ((table && name == kRequirePartitionFilter &&
          BoolOption(*value, metadata.require_partition_filter)) ||
         (table && value->is_null() && (name == kPartitionExpirationDays || name == kKmsKeyName)) ||
         (name == kDescription && StringOption(*value, metadata.description)) ||
         (name == kFriendlyName && StringOption(*value, metadata.friendly_name)) ||
         (name == kLabels && LabelsOption(*value, metadata.labels, metadata.label_order)) ||
         (name == kTags && TagsOption(*value, metadata.resource_tags, metadata.tag_order)) ||
         (name == kExpirationTimestamp && ExpirationOption(*value, metadata.expiration_time)) ||
         (table && name == kDefaultRoundingMode &&
          RoundingModeOption(*value, metadata.default_rounding_mode)));
    if (!applied) {
      return Unsupported(
          scope, std::string(DdlOptionContextName(statement)) + " option " + option->name());
    }
  }
  return metadata;
}

// The description, friendly name, labels, default table expiration, default rounding mode and
// location the OPTIONS of a CREATE SCHEMA set. The other options are unsupported, since they change
// how BigQuery treats the dataset's tables. Throws ApiError for a location BigQuery does not have.
std::optional<DatasetMetadata> SchemaOptionsMetadata(const Options& options, const Scope& scope) {
  using enum DdlOptionId;
  DatasetMetadata metadata;
  for (const auto& option : options) {
    const auto name = ParseDdlOptionId(ToLowerAscii(option->name()));
    const auto value = OptionValue(*option, DdlOptionContext::kCreateSchema, scope);
    const bool applied =
        value &&
        ((name == kDescription && StringOption(*value, metadata.description)) ||
         (name == kFriendlyName && StringOption(*value, metadata.friendly_name)) ||
         (name == kLabels && LabelsOption(*value, metadata.labels, metadata.label_order)) ||
         (name == kTags && TagsOption(*value, metadata.resource_tags, metadata.tag_order)) ||
         (name == kDefaultTableExpirationDays &&
          DaysOption(*value, metadata.default_table_expiration_ms)) ||
         (name == kDefaultRoundingMode &&
          RoundingModeOption(*value, metadata.default_rounding_mode)) ||
         (name == kLocation && LocationOption(*value, metadata.location)));
    if (!applied) {
      return Unsupported(scope, "CREATE SCHEMA option " + option->name());
    }
  }
  return metadata;
}

// The metadata SET OPTIONS of `statement` sets on `updates`. The
// other options change how BigQuery stores or reads the table or dataset, which the
// emulator does not emulate, so they are unsupported. Tag removal also accepts -=.
bool SetOptions(const Options& options, DdlOptionContext statement, const Scope& scope,
                OptionUpdates& updates) {
  using enum DdlOptionId;
  bool tags_seen = false;
  const bool table = statement == DdlOptionContext::kAlterTable;
  const bool schema = statement == DdlOptionContext::kAlterSchema;
  for (const auto& option : options) {
    const auto name = ParseDdlOptionId(ToLowerAscii(option->name()));
    if (schema && name == kLocation) {
      throw ApiError::InvalidQuery("Unknown option: " + option->name());
    }
    if (name == kTags) {
      if (tags_seen) {
        Unsupported(scope, "repeated tags in SET OPTIONS");
        return false;
      }
      tags_seen = true;
    }
    const auto value = OptionValue(*option, statement, scope);
    const auto fail = [&] {
      Unsupported(scope,
                  std::string(DdlOptionContextName(statement)) + " option " + option->name());
      return false;
    };
    if (name == kTags && option->assignment_op() == googlesql::ResolvedOption::SUB_ASSIGN) {
      std::vector<std::string> order;
      if (!value || value->is_null() || !TagsOption(*value, updates.removed_tags, order)) {
        return fail();
      }
      continue;
    }
    const bool applied =
        value &&
        ((table && name == kRequirePartitionFilter &&
          BoolOption(*value, updates.require_partition_filter.emplace())) ||
         (name == kDescription && StringOption(*value, updates.description.emplace())) ||
         (name == kFriendlyName && StringOption(*value, updates.friendly_name.emplace())) ||
         (name == kLabels && LabelsOption(*value, updates.labels.emplace(), updates.label_order)) ||
         (name == kTags &&
          TagsOption(*value, updates.resource_tags.emplace(), updates.tag_order)) ||
         (table && name == kExpirationTimestamp &&
          ExpirationOption(*value, updates.expiration_time.emplace())) ||
         (schema && name == kDefaultTableExpirationDays &&
          DaysOption(*value, updates.default_table_expiration_ms.emplace())) ||
         ((table || schema) && name == kDefaultRoundingMode &&
          RoundingModeOption(*value, updates.default_rounding_mode.emplace())));
    if (!applied) {
      return fail();
    }
  }
  return true;
}

}  // namespace bigquery_emulator_duckdb::translator
