#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace googlesql {
class Type;
class Value;
}  // namespace googlesql

namespace bigquery_emulator_duckdb {

enum class DdlOptionContext : uint8_t {
  kCreateTable,
  kAlterTable,
  kCreateSchema,
  kAlterSchema,
  kCreateView,
  kCreateMaterializedView,
  kCreateSnapshotTable,
  kCreateExternalTable,
  kColumn,
  kCreateFunction,
  kCreateTableFunction,
  kCreateProcedure,
};
enum class DdlOptionId : uint8_t {
  kDescription,
  kFriendlyName,
  kLabels,
  kTags,
  kExpirationTimestamp,
  kRequirePartitionFilter,
  kDefaultRoundingMode,
  kPartitionExpirationDays,
  kKmsKeyName,
  kDefaultTableExpirationDays,
  kLocation,
  kRoundingMode,
  kRuntimeVersion,
  kEntryPoint,
  kStrictMode,
  kFormat,
  kUris,
  kEnableListInference,
  kSkipLeadingRows,
  kFieldDelimiter,
};
std::string_view DdlOptionContextName(DdlOptionContext context);
std::string_view DdlOptionName(DdlOptionId option);
std::optional<DdlOptionContext> ParseDdlOptionContext(std::string_view name);
std::optional<DdlOptionId> ParseDdlOptionId(std::string_view name);

// Types of the options the DDL handlers implement. The catalog test checks these
// against BigQuery's option catalog; they do not determine support.
enum class DdlOptionType : uint8_t {
  kString,
  kBool,
  kInt64,
  kFloat64,
  kTimestamp,
  kLabels,
  kStrings,
};
struct DdlOptionDefinition {
  DdlOptionContext context;
  DdlOptionId name;
  DdlOptionType type;
};
std::span<const DdlOptionDefinition> DdlOptionDefinitions();
const DdlOptionDefinition* FindDdlOption(DdlOptionContext context, DdlOptionId name);
const DdlOptionDefinition* FindDdlOption(DdlOptionContext context, std::string_view name);
const googlesql::Type* DdlOptionSqlType(DdlOptionType type);
// Preserves the handlers' existing acceptance of untyped NULL and INT64 expiration days.
bool DdlOptionValueMatches(DdlOptionType type, const googlesql::Value& value);

}  // namespace bigquery_emulator_duckdb
