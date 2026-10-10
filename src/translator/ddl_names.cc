#include "src/translator/ddl_names.h"

#include <optional>
#include <string>
#include <vector>

#include "src/catalog.h"
#include "src/references.h"
#include "src/translated_statement.h"
#include "src/translator/context.h"

namespace bigquery_emulator_duckdb::translator {

// DDL names tables and datasets by path rather than through the catalog, so the defaults the
// catalog resolves queries with are applied here. The table or dataset named is the statement's
// DDL target, which is recorded in the context. The table a CREATE statement creates is
// temporary only with TEMP, so `create` leaves temporary tables out.
std::optional<std::string> TargetTable(const std::vector<std::string>& path, const Scope& scope,
                                       bool create) {
  const DefaultDataset& defaults = scope.context.defaults;
  const auto parts = ResolveTablePath(path, defaults.project, defaults.dataset,
                                      create ? nullptr : defaults.temporary);
  if (parts.empty()) {
    return Unsupported(scope, "table name " + Join(path, "."));
  }
  scope.context.ddl_target_table =
      TableReference{.project_id = parts.at(0), .dataset_id = parts.at(1), .table_id = parts.at(2)};
  return QualifiedName(*scope.context.ddl_target_table);
}

std::optional<std::string> TargetDataset(const std::vector<std::string>& path, const Scope& scope) {
  // Reuse table path normalization so dots inside a domain-scoped project are handled
  // the same way for CREATE/DROP SCHEMA and table references.
  std::vector<std::string> table_path = path;
  table_path.emplace_back("_dataset_path_placeholder");
  const auto parts = NormalizeTablePath(table_path, scope.context.defaults.project,
                                        scope.context.defaults.dataset);
  if (path.empty() || parts.empty()) {
    return Unsupported(scope, "dataset name " + Join(path, "."));
  }
  scope.context.ddl_target_dataset =
      DatasetReference{.project_id = parts.at(0), .dataset_id = parts.at(1)};
  return QualifiedName(*scope.context.ddl_target_dataset);
}

}  // namespace bigquery_emulator_duckdb::translator
