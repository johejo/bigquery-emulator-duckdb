#include "src/ddl_option_types.h"

#include <algorithm>
#include <array>
#include <span>
#include <string_view>

#include "googlesql/public/type.h"
#include "googlesql/public/types/type_factory.h"
#include "googlesql/public/value.h"

namespace bigquery_emulator_duckdb {
namespace {
using enum DdlOptionType;
constexpr auto kOptions = std::to_array<DdlOptionDefinition>({
    {.context = "CREATE TABLE", .name = "description", .type = kString},
    {.context = "CREATE TABLE", .name = "friendly_name", .type = kString},
    {.context = "CREATE TABLE", .name = "labels", .type = kLabels},
    {.context = "CREATE TABLE", .name = "expiration_timestamp", .type = kTimestamp},
    {.context = "CREATE TABLE", .name = "require_partition_filter", .type = kBool},
    {.context = "CREATE TABLE", .name = "default_rounding_mode", .type = kString},
    {.context = "CREATE TABLE", .name = "partition_expiration_days", .type = kFloat64},
    {.context = "CREATE TABLE", .name = "kms_key_name", .type = kString},
    {.context = "ALTER TABLE", .name = "description", .type = kString},
    {.context = "ALTER TABLE", .name = "friendly_name", .type = kString},
    {.context = "ALTER TABLE", .name = "labels", .type = kLabels},
    {.context = "ALTER TABLE", .name = "expiration_timestamp", .type = kTimestamp},
    {.context = "ALTER TABLE", .name = "require_partition_filter", .type = kBool},
    {.context = "ALTER TABLE", .name = "default_rounding_mode", .type = kString},
    {.context = "CREATE SCHEMA", .name = "description", .type = kString},
    {.context = "CREATE SCHEMA", .name = "friendly_name", .type = kString},
    {.context = "CREATE SCHEMA", .name = "labels", .type = kLabels},
    {.context = "CREATE SCHEMA", .name = "default_table_expiration_days", .type = kFloat64},
    {.context = "CREATE SCHEMA", .name = "default_rounding_mode", .type = kString},
    {.context = "CREATE SCHEMA", .name = "location", .type = kString},
    {.context = "ALTER SCHEMA", .name = "description", .type = kString},
    {.context = "ALTER SCHEMA", .name = "friendly_name", .type = kString},
    {.context = "ALTER SCHEMA", .name = "labels", .type = kLabels},
    {.context = "ALTER SCHEMA", .name = "default_table_expiration_days", .type = kFloat64},
    {.context = "ALTER SCHEMA", .name = "default_rounding_mode", .type = kString},
    {.context = "CREATE VIEW", .name = "description", .type = kString},
    {.context = "CREATE VIEW", .name = "friendly_name", .type = kString},
    {.context = "CREATE VIEW", .name = "labels", .type = kLabels},
    {.context = "CREATE VIEW", .name = "expiration_timestamp", .type = kTimestamp},
    {.context = "CREATE MATERIALIZED VIEW", .name = "description", .type = kString},
    {.context = "CREATE MATERIALIZED VIEW", .name = "friendly_name", .type = kString},
    {.context = "CREATE MATERIALIZED VIEW", .name = "labels", .type = kLabels},
    {.context = "CREATE MATERIALIZED VIEW", .name = "expiration_timestamp", .type = kTimestamp},
    {.context = "CREATE SNAPSHOT TABLE", .name = "description", .type = kString},
    {.context = "CREATE SNAPSHOT TABLE", .name = "friendly_name", .type = kString},
    {.context = "CREATE SNAPSHOT TABLE", .name = "labels", .type = kLabels},
    {.context = "CREATE SNAPSHOT TABLE", .name = "expiration_timestamp", .type = kTimestamp},
    {.context = "column", .name = "description", .type = kString},
    {.context = "column", .name = "rounding_mode", .type = kString},
    {.context = "CREATE FUNCTION", .name = "description", .type = kString},
    {.context = "CREATE TABLE FUNCTION", .name = "description", .type = kString},
    {.context = "CREATE PROCEDURE", .name = "description", .type = kString},
    {.context = "CREATE PROCEDURE", .name = "strict_mode", .type = kBool},
});
}  // namespace

std::span<const DdlOptionDefinition> DdlOptionDefinitions() { return kOptions; }

const DdlOptionDefinition* FindDdlOption(std::string_view context, std::string_view name) {
  const auto* found = std::ranges::find_if(kOptions, [&](const auto& option) {
    return option.context == context && option.name == name;
  });
  return found == kOptions.end() ? nullptr : found;
}

const googlesql::Type* DdlOptionSqlType(DdlOptionType type) {
  switch (type) {
    case kString:
      return googlesql::types::StringType();
    case kBool:
      return googlesql::types::BoolType();
    case kFloat64:
      return googlesql::types::DoubleType();
    case kTimestamp:
      return googlesql::types::TimestampType();
    case kLabels: {
      static const googlesql::ArrayType* labels = []() -> const googlesql::ArrayType* {
        static googlesql::TypeFactory factory;
        const googlesql::StructType* pair = nullptr;
        if (!factory
                 .MakeStructType(
                     {{"", googlesql::types::StringType()}, {"", googlesql::types::StringType()}},
                     &pair)
                 .ok()) {
          return nullptr;
        }
        const auto array = factory.MakeArrayType(pair);
        return array.ok() ? *array : nullptr;
      }();
      return labels;
    }
  }
  return nullptr;
}

bool DdlOptionValueMatches(DdlOptionType type, const googlesql::Value& value) {
  if (value.is_null() || (type == kFloat64 && value.type()->IsInt64())) {
    return true;
  }
  const auto* expected = DdlOptionSqlType(type);
  // Equivalent ignores STRUCT field names, as the labels handler does.
  return expected != nullptr && value.type()->Equivalent(expected);
}
}  // namespace bigquery_emulator_duckdb
