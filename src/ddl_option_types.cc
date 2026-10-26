#include "src/ddl_option_types.h"

#include <algorithm>
#include <array>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

#include "googlesql/public/type.h"
#include "googlesql/public/types/type_factory.h"
#include "googlesql/public/value.h"

namespace bigquery_emulator_duckdb {
namespace {
using C = DdlOptionContext;
using I = DdlOptionId;
using enum DdlOptionType;
constexpr auto kContexts = std::to_array<std::pair<C, std::string_view>>({
    {C::kCreateTable, "CREATE TABLE"},
    {C::kAlterTable, "ALTER TABLE"},
    {C::kCreateSchema, "CREATE SCHEMA"},
    {C::kAlterSchema, "ALTER SCHEMA"},
    {C::kCreateView, "CREATE VIEW"},
    {C::kCreateMaterializedView, "CREATE MATERIALIZED VIEW"},
    {C::kCreateSnapshotTable, "CREATE SNAPSHOT TABLE"},
    {C::kColumn, "column"},
    {C::kCreateFunction, "CREATE FUNCTION"},
    {C::kCreateTableFunction, "CREATE TABLE FUNCTION"},
    {C::kCreateProcedure, "CREATE PROCEDURE"},
});
constexpr auto kNames = std::to_array<std::pair<I, std::string_view>>({
    {I::kDescription, "description"},
    {I::kFriendlyName, "friendly_name"},
    {I::kLabels, "labels"},
    {I::kTags, "tags"},
    {I::kExpirationTimestamp, "expiration_timestamp"},
    {I::kRequirePartitionFilter, "require_partition_filter"},
    {I::kDefaultRoundingMode, "default_rounding_mode"},
    {I::kPartitionExpirationDays, "partition_expiration_days"},
    {I::kKmsKeyName, "kms_key_name"},
    {I::kDefaultTableExpirationDays, "default_table_expiration_days"},
    {I::kLocation, "location"},
    {I::kRoundingMode, "rounding_mode"},
    {I::kRuntimeVersion, "runtime_version"},
    {I::kEntryPoint, "entry_point"},
    {I::kStrictMode, "strict_mode"},
});
constexpr auto kOptions = std::to_array<DdlOptionDefinition>({
    {.context = C::kCreateTable, .name = I::kDescription, .type = kString},
    {.context = C::kCreateTable, .name = I::kFriendlyName, .type = kString},
    {.context = C::kCreateTable, .name = I::kLabels, .type = kLabels},
    {.context = C::kCreateTable, .name = I::kTags, .type = kLabels},
    {.context = C::kCreateTable, .name = I::kExpirationTimestamp, .type = kTimestamp},
    {.context = C::kCreateTable, .name = I::kRequirePartitionFilter, .type = kBool},
    {.context = C::kCreateTable, .name = I::kDefaultRoundingMode, .type = kString},
    {.context = C::kCreateTable, .name = I::kPartitionExpirationDays, .type = kFloat64},
    {.context = C::kCreateTable, .name = I::kKmsKeyName, .type = kString},
    {.context = C::kAlterTable, .name = I::kDescription, .type = kString},
    {.context = C::kAlterTable, .name = I::kFriendlyName, .type = kString},
    {.context = C::kAlterTable, .name = I::kLabels, .type = kLabels},
    {.context = C::kAlterTable, .name = I::kTags, .type = kLabels},
    {.context = C::kAlterTable, .name = I::kExpirationTimestamp, .type = kTimestamp},
    {.context = C::kAlterTable, .name = I::kRequirePartitionFilter, .type = kBool},
    {.context = C::kAlterTable, .name = I::kDefaultRoundingMode, .type = kString},
    {.context = C::kCreateSchema, .name = I::kDescription, .type = kString},
    {.context = C::kCreateSchema, .name = I::kFriendlyName, .type = kString},
    {.context = C::kCreateSchema, .name = I::kLabels, .type = kLabels},
    {.context = C::kCreateSchema, .name = I::kTags, .type = kLabels},
    {.context = C::kCreateSchema, .name = I::kDefaultTableExpirationDays, .type = kFloat64},
    {.context = C::kCreateSchema, .name = I::kDefaultRoundingMode, .type = kString},
    {.context = C::kCreateSchema, .name = I::kLocation, .type = kString},
    {.context = C::kAlterSchema, .name = I::kDescription, .type = kString},
    {.context = C::kAlterSchema, .name = I::kFriendlyName, .type = kString},
    {.context = C::kAlterSchema, .name = I::kLabels, .type = kLabels},
    {.context = C::kAlterSchema, .name = I::kTags, .type = kLabels},
    {.context = C::kAlterSchema, .name = I::kDefaultTableExpirationDays, .type = kFloat64},
    {.context = C::kAlterSchema, .name = I::kDefaultRoundingMode, .type = kString},
    {.context = C::kCreateView, .name = I::kDescription, .type = kString},
    {.context = C::kCreateView, .name = I::kFriendlyName, .type = kString},
    {.context = C::kCreateView, .name = I::kLabels, .type = kLabels},
    {.context = C::kCreateView, .name = I::kTags, .type = kLabels},
    {.context = C::kCreateView, .name = I::kExpirationTimestamp, .type = kTimestamp},
    {.context = C::kCreateMaterializedView, .name = I::kDescription, .type = kString},
    {.context = C::kCreateMaterializedView, .name = I::kFriendlyName, .type = kString},
    {.context = C::kCreateMaterializedView, .name = I::kLabels, .type = kLabels},
    {.context = C::kCreateMaterializedView, .name = I::kTags, .type = kLabels},
    {.context = C::kCreateMaterializedView, .name = I::kExpirationTimestamp, .type = kTimestamp},
    {.context = C::kCreateSnapshotTable, .name = I::kDescription, .type = kString},
    {.context = C::kCreateSnapshotTable, .name = I::kFriendlyName, .type = kString},
    {.context = C::kCreateSnapshotTable, .name = I::kLabels, .type = kLabels},
    {.context = C::kCreateSnapshotTable, .name = I::kTags, .type = kLabels},
    {.context = C::kCreateSnapshotTable, .name = I::kExpirationTimestamp, .type = kTimestamp},
    {.context = C::kColumn, .name = I::kDescription, .type = kString},
    {.context = C::kColumn, .name = I::kRoundingMode, .type = kString},
    {.context = C::kCreateFunction, .name = I::kDescription, .type = kString},
    {.context = C::kCreateFunction, .name = I::kRuntimeVersion, .type = kString},
    {.context = C::kCreateFunction, .name = I::kEntryPoint, .type = kString},
    {.context = C::kCreateTableFunction, .name = I::kDescription, .type = kString},
    {.context = C::kCreateProcedure, .name = I::kDescription, .type = kString},
    {.context = C::kCreateProcedure, .name = I::kStrictMode, .type = kBool},
});
}  // namespace

std::string_view DdlOptionContextName(C context) {
  for (const auto& [id, name] : kContexts) {
    if (id == context) {
      return name;
    }
  }
  return "";
}
std::string_view DdlOptionName(I option) {
  for (const auto& [id, spelling] : kNames) {
    if (id == option) {
      return spelling;
    }
  }
  return "";
}
std::optional<C> ParseDdlOptionContext(std::string_view name) {
  for (const auto& [id, spelling] : kContexts) {
    if (spelling == name) {
      return id;
    }
  }
  return std::nullopt;
}
std::optional<I> ParseDdlOptionId(std::string_view name) {
  for (const auto& [id, spelling] : kNames) {
    if (spelling == name) {
      return id;
    }
  }
  return std::nullopt;
}
std::span<const DdlOptionDefinition> DdlOptionDefinitions() { return kOptions; }
const DdlOptionDefinition* FindDdlOption(C context, std::string_view name) {
  const auto id = ParseDdlOptionId(name);
  const auto* found = std::ranges::find_if(
      kOptions, [&](const auto& option) { return option.context == context && option.name == id; });
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
