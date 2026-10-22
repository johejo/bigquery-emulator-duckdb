#pragma once

#include <cstdint>
#include <span>
#include <string_view>

namespace googlesql {
class Type;
class Value;
}  // namespace googlesql

namespace bigquery_emulator_duckdb {

// Types of the options the DDL handlers implement. tests/ddl_options_catalog_test.cc compares them
// with tools/ddl_options.json, the catalog of BigQuery's options; they do not determine support.
enum class DdlOptionType : uint8_t { kString, kBool, kFloat64, kTimestamp, kLabels };

struct DdlOptionDefinition {
  std::string_view context;
  std::string_view name;
  DdlOptionType type;
};

std::span<const DdlOptionDefinition> DdlOptionDefinitions();
const DdlOptionDefinition* FindDdlOption(std::string_view context, std::string_view name);
const googlesql::Type* DdlOptionSqlType(DdlOptionType type);
// Preserves the handlers' existing acceptance of untyped NULL and INT64 expiration days.
bool DdlOptionValueMatches(DdlOptionType type, const googlesql::Value& value);

}  // namespace bigquery_emulator_duckdb
