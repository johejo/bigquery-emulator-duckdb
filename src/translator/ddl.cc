#include "src/translator/ddl.h"

#include <optional>
#include <string>
#include <vector>

#include "googlesql/resolved_ast/resolved_ast.h"
#include "src/catalog.h"
#include "src/duckdb_sql.h"
#include "src/references.h"
#include "src/translated_statement.h"
#include "src/translator/context.h"
#include "src/translator/ddl_internal.h"

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
  scope.context.ddl_target_table = TableReference{parts.at(0), parts.at(1), parts.at(2)};
  return QualifiedName(*scope.context.ddl_target_table);
}

// The temporary table CREATE TEMP TABLE `path` creates. The analyzer rejects one outside a
// multi-statement query, and one with a qualified name.
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
  scope.context.ddl_target_dataset = DatasetReference{parts.at(0), parts.at(1)};
  return QualifiedName(*scope.context.ddl_target_dataset);
}

std::optional<std::string> Drop(const googlesql::ResolvedDropStmt& drop, const Scope& scope) {
  const std::string object_type = ToUpperAscii(drop.object_type());
  const bool is_schema = object_type == "SCHEMA";
  if (object_type != "TABLE" && object_type != "VIEW" && !is_schema) {
    return Unsupported(scope, "DROP " + object_type);
  }
  const auto path =
      is_schema ? TargetDataset(drop.name_path(), scope) : TargetTable(drop.name_path(), scope);
  if (!path) {
    return std::nullopt;
  }
  std::string sql = "DROP " + object_type + (drop.is_if_exists() ? " IF EXISTS " : " ") + *path;
  switch (drop.drop_mode()) {
    case googlesql::ResolvedDropStmt::CASCADE:
      return sql + " CASCADE";
    case googlesql::ResolvedDropStmt::RESTRICT:
      return sql + " RESTRICT";
    default:
      return sql;
  }
}

}  // namespace bigquery_emulator_duckdb::translator
