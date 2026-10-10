#pragma once

// Shared DDL name resolution, metadata options and column definitions. Object-specific SQL
// generation stays in ddl_table.cc, ddl_view.cc, ddl_schema.cc and ddl_routine.cc.
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "googlesql/resolved_ast/resolved_ast.h"
#include "src/field_schema.h"
#include "src/table_metadata.h"
#include "src/translated_statement.h"
#include "src/translator/context.h"

namespace bigquery_emulator_duckdb::translator {

using Options = std::vector<std::unique_ptr<const googlesql::ResolvedOption>>;

std::optional<std::string> TargetTable(const std::vector<std::string>& path, const Scope& scope,
                                       bool create = false);
std::optional<std::string> TargetDataset(const std::vector<std::string>& path, const Scope& scope);
bool IfNotExists(const googlesql::ResolvedCreateStatement& create);
const googlesql::Value* OptionLiteral(const googlesql::ResolvedOption& option);
bool StringOption(const googlesql::Value& value, std::string& into);
std::optional<TableMetadata> OptionsMetadata(const Options& options, std::string_view statement,
                                             const Scope& scope, TableMetadata metadata = {});
std::optional<DatasetMetadata> SchemaOptionsMetadata(const Options& options, const Scope& scope);
bool SetOptions(const Options& options, std::string_view statement, const Scope& scope,
                OptionUpdates& updates);
std::optional<std::string> ColumnDefinitionType(const googlesql::ResolvedColumnDefinition& column,
                                                const Scope& scope);
bool ColumnSetOptions(const Options& options, const Scope& scope, ColumnOptionsAction& action);
std::optional<FieldSchema> ColumnField(const googlesql::ResolvedColumnDefinition& column,
                                       const Scope& scope);

}  // namespace bigquery_emulator_duckdb::translator
