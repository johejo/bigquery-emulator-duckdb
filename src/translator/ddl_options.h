#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "googlesql/public/value.h"
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
                                            std::string_view object, const Scope& scope);
bool StringOption(const googlesql::Value& value, std::string& into);
// Sets `into` to the rounding mode `value` names, or clears it for NULL. False for any other
// value.
bool RoundingModeOption(const googlesql::Value& value, std::string& into);
std::optional<TableMetadata> OptionsMetadata(const Options& options, std::string_view statement,
                                             const Scope& scope, TableMetadata metadata = {});
std::optional<DatasetMetadata> SchemaOptionsMetadata(const Options& options, const Scope& scope);
bool SetOptions(const Options& options, std::string_view statement, const Scope& scope,
                OptionUpdates& updates);

}  // namespace bigquery_emulator_duckdb::translator
