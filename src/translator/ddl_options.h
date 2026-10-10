#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "src/table_metadata.h"

namespace googlesql {
class ResolvedOption;
class Value;
}  // namespace googlesql

namespace bigquery_emulator_duckdb {
struct OptionUpdates;
}  // namespace bigquery_emulator_duckdb

namespace bigquery_emulator_duckdb::translator {
struct Scope;

using Options = std::vector<std::unique_ptr<const googlesql::ResolvedOption>>;

const googlesql::Value* OptionLiteral(const googlesql::ResolvedOption& option);
bool StringOption(const googlesql::Value& value, std::string& into);
std::optional<TableMetadata> OptionsMetadata(const Options& options, std::string_view statement,
                                             const Scope& scope, TableMetadata metadata = {});
std::optional<DatasetMetadata> SchemaOptionsMetadata(const Options& options, const Scope& scope);
bool SetOptions(const Options& options, std::string_view statement, const Scope& scope,
                OptionUpdates& updates);

}  // namespace bigquery_emulator_duckdb::translator
