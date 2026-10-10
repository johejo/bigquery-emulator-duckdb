#include <optional>
#include <string>
#include <utility>

#include "googlesql/resolved_ast/resolved_ast.h"
#include "src/references.h"
#include "src/table_metadata.h"
#include "src/translated_statement.h"
#include "src/translator/context.h"
#include "src/translator/ddl.h"
#include "src/translator/ddl_internal.h"

namespace bigquery_emulator_duckdb::translator {
namespace {}  // namespace

std::optional<std::string> CreateSchema(const googlesql::ResolvedCreateSchemaStmt& create,
                                        const Scope& scope) {
  if (create.collation_name() != nullptr) {
    return Unsupported(scope, "dataset collation");
  }
  const auto path = TargetDataset(create.name_path(), scope);
  const std::optional<DatasetReference>& dataset = scope.context.ddl_target_dataset;
  if (!path || !dataset) {
    return std::nullopt;
  }
  // The description, friendly name and labels the OPTIONS of a CREATE SCHEMA set. Every dataset is
  // in the US, so a location elsewhere is unsupported, and so are the other options, which change
  // how BigQuery treats the dataset's tables.
  std::optional<DatasetMetadata> metadata = SchemaOptionsMetadata(create.option_list(), scope);
  if (!metadata) {
    return std::nullopt;
  }
  scope.context.dataset = DatasetDefinition{
      .dataset = *dataset,
      .metadata = *std::move(metadata),
      .if_not_exists = IfNotExists(create),
  };
  switch (create.create_mode()) {
    case googlesql::ResolvedCreateStatement::CREATE_OR_REPLACE:
      return Unsupported(scope, "CREATE OR REPLACE SCHEMA");
    case googlesql::ResolvedCreateStatement::CREATE_IF_NOT_EXISTS:
      return "CREATE SCHEMA IF NOT EXISTS " + *path;
    default:
      return "CREATE SCHEMA " + *path;
  }
}

// ALTER SCHEMA SET OPTIONS, which the emulator applies to the dataset as it is when the statement
// runs.
std::optional<std::string> AlterSchema(const googlesql::ResolvedAlterSchemaStmt& alter,
                                       const Scope& scope) {
  const auto path = TargetDataset(alter.name_path(), scope);
  const std::optional<DatasetReference>& dataset = scope.context.ddl_target_dataset;
  if (!path || !dataset) {
    return std::nullopt;
  }
  DatasetAlteration alteration{.dataset = *dataset, .if_exists = alter.is_if_exists()};
  for (const auto& action : alter.alter_action_list()) {
    if (!action->Is<googlesql::ResolvedSetOptionsAction>()) {
      return Unsupported(scope, "ALTER SCHEMA action " + action->node_kind_string());
    }
    if (!SetOptions(action->GetAs<googlesql::ResolvedSetOptionsAction>()->option_list(),
                    "ALTER SCHEMA", scope, alteration.options)) {
      return std::nullopt;
    }
  }
  scope.context.altered_dataset = std::move(alteration);
  return "";
}

}  // namespace bigquery_emulator_duckdb::translator
