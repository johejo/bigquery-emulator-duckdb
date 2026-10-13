#include "src/translator/ddl_options.h"

#include <cmath>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>

#include "absl/time/time.h"
#include "googlesql/public/type.h"
#include "googlesql/public/value.h"
#include "googlesql/resolved_ast/resolved_ast.h"
#include "src/duckdb_sql.h"
#include "src/table_metadata.h"
#include "src/translated_statement.h"
#include "src/translator/context.h"

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

}  // namespace

// The literal an option of a CREATE statement assigns, or nullptr for any other value.
const googlesql::Value* OptionLiteral(const googlesql::ResolvedOption& option) {
  if (!option.qualifier().empty() ||
      option.assignment_op() != googlesql::ResolvedOption::DEFAULT_ASSIGN ||
      !option.value()->Is<googlesql::ResolvedLiteral>()) {
    return nullptr;
  }
  return &option.value()->GetAs<googlesql::ResolvedLiteral>()->value();
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

// The metadata the OPTIONS of a CREATE TABLE or CREATE VIEW set.
std::optional<TableMetadata> OptionsMetadata(const Options& options, std::string_view statement,
                                             const Scope& scope, TableMetadata metadata) {
  for (const auto& option : options) {
    const std::string name = ToLowerAscii(option->name());
    const googlesql::Value* value = OptionLiteral(*option);
    const bool applied =
        value != nullptr &&
        ((name == "description" && StringOption(*value, metadata.description)) ||
         (name == "friendly_name" && StringOption(*value, metadata.friendly_name)) ||
         (name == "labels" && LabelsOption(*value, metadata.labels)) ||
         (name == "expiration_timestamp" && ExpirationOption(*value, metadata.expiration_time)));
    if (!applied) {
      return Unsupported(scope, std::string(statement) + " option " + option->name());
    }
  }
  return metadata;
}

// The description, friendly name, labels and default table expiration the OPTIONS of a CREATE
// SCHEMA set. Every dataset is in the US, so a location elsewhere is unsupported, and so are the
// other options, which change how BigQuery treats the dataset's tables.
std::optional<DatasetMetadata> SchemaOptionsMetadata(const Options& options, const Scope& scope) {
  DatasetMetadata metadata;
  for (const auto& option : options) {
    const std::string name = ToLowerAscii(option->name());
    const googlesql::Value* value = OptionLiteral(*option);
    std::string location;
    const bool applied =
        value != nullptr &&
        ((name == "description" && StringOption(*value, metadata.description)) ||
         (name == "friendly_name" && StringOption(*value, metadata.friendly_name)) ||
         (name == "labels" && LabelsOption(*value, metadata.labels)) ||
         (name == "default_table_expiration_days" &&
          DaysOption(*value, metadata.default_table_expiration_ms)) ||
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
    const googlesql::Value* value = OptionLiteral(*option);
    const bool applied =
        value != nullptr &&
        ((name == "description" && StringOption(*value, updates.description.emplace())) ||
         (name == "friendly_name" && StringOption(*value, updates.friendly_name.emplace())) ||
         (name == "labels" && LabelsOption(*value, updates.labels.emplace())) ||
         (statement == "ALTER TABLE" && name == "expiration_timestamp" &&
          ExpirationOption(*value, updates.expiration_time.emplace())) ||
         (statement == "ALTER SCHEMA" && name == "default_table_expiration_days" &&
          DaysOption(*value, updates.default_table_expiration_ms.emplace())));
    if (!applied) {
      Unsupported(scope, std::string(statement) + " option " + option->name());
      return false;
    }
  }
  return true;
}

}  // namespace bigquery_emulator_duckdb::translator
