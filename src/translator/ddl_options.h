#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "googlesql/public/value.h"
#include "src/ddl_option_types.h"
#include "src/field_schema.h"
#include "src/table_metadata.h"

namespace googlesql {
class ResolvedOption;
}  // namespace googlesql

namespace bigquery_emulator_duckdb {
struct OptionUpdates;
}  // namespace bigquery_emulator_duckdb

namespace bigquery_emulator_duckdb::translator {
struct Scope;

using Options = std::vector<std::unique_ptr<const googlesql::ResolvedOption>>;

std::optional<googlesql::Value> OptionValue(const googlesql::ResolvedOption& option,
                                            DdlOptionContext context, const Scope& scope);
bool StringOption(const googlesql::Value& value, std::string& into);
// Sets `into` to the rounding mode `value` names, or clears it for NULL. False for any other
// value.
bool RoundingModeOption(const googlesql::Value& value, RoundingMode& into);
std::optional<TableMetadata> OptionsMetadata(const Options& options, DdlOptionContext statement,
                                             const Scope& scope, TableMetadata metadata = {});
std::optional<DatasetMetadata> SchemaOptionsMetadata(const Options& options, const Scope& scope);
bool SetOptions(const Options& options, DdlOptionContext statement, const Scope& scope,
                OptionUpdates& updates);

}  // namespace bigquery_emulator_duckdb::translator
