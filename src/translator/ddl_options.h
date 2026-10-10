#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "googlesql/resolved_ast/resolved_ast.h"
#include "src/table_metadata.h"
#include "src/translated_statement.h"
#include "src/translator/context.h"

namespace bigquery_emulator_duckdb::translator {

using Options = std::vector<std::unique_ptr<const googlesql::ResolvedOption>>;

const googlesql::Value* OptionLiteral(const googlesql::ResolvedOption& option);
bool StringOption(const googlesql::Value& value, std::string& into);
std::optional<TableMetadata> OptionsMetadata(const Options& options, std::string_view statement,
                                             const Scope& scope, TableMetadata metadata = {});
std::optional<DatasetMetadata> SchemaOptionsMetadata(const Options& options, const Scope& scope);
bool SetOptions(const Options& options, std::string_view statement, const Scope& scope,
                OptionUpdates& updates);

}  // namespace bigquery_emulator_duckdb::translator
