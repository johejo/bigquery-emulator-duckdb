#include "src/translator/ddl.h"

#include <optional>
#include <string>

#include "googlesql/resolved_ast/resolved_ast.h"
#include "src/duckdb_sql.h"
#include "src/references.h"
#include "src/table_path.h"
#include "src/translated_statement.h"
#include "src/translator/context.h"
#include "src/translator/ddl_names.h"

namespace bigquery_emulator_duckdb::translator {

std::optional<std::string> Drop(const googlesql::ResolvedDropStmt& drop, const Scope& scope) {
  const std::string object_type = ToUpperAscii(drop.object_type());
  if (object_type == "PROCEDURE") {
    return DropProcedure(drop, scope);
  }
  if (object_type == "PROPERTY GRAPH") {
    const auto parts =
        NormalizeTablePath(drop.name_path(), scope.context.defaults.project,
                           scope.context.defaults.dataset, scope.context.defaults.source);
    if (parts.size() != 3) {
      return Unsupported(scope, "property graph name without a dataset");
    }
    scope.context.statement.skips_property_graph = drop.is_if_exists();
    scope.context.statement.property_graph = TableReference{
        .project_id = parts.at(0),
        .dataset_id = parts.at(1),
        .table_id = parts.at(2),
    };
    return "DROP PROPERTY GRAPH " + DuckPGQName(parts);
  }
  const bool is_schema = object_type == "SCHEMA";
  // An external table is a DuckDB table; DROP EXTERNAL TABLE drops only external tables, which
  // the emulator checks when it runs.
  const bool external = object_type == "EXTERNAL TABLE";
  if (object_type != "TABLE" && object_type != "VIEW" && !is_schema && !external) {
    return Unsupported(scope, "DROP " + object_type);
  }
  const auto path =
      is_schema ? TargetDataset(drop.name_path(), scope) : TargetTable(drop.name_path(), scope);
  if (!path) {
    return std::nullopt;
  }
  std::string sql = "DROP " + std::string(external ? "TABLE" : object_type) +
                    (drop.is_if_exists() ? " IF EXISTS " : " ") + *path;
  switch (drop.drop_mode()) {
    case googlesql::ResolvedDropStmt::CASCADE:
      return sql + " CASCADE";
    case googlesql::ResolvedDropStmt::RESTRICT:
      return sql + " RESTRICT";
    default:
      return sql;
  }
}

std::optional<std::string> DropMaterializedView(
    const googlesql::ResolvedDropMaterializedViewStmt& drop, const Scope& scope) {
  const auto path = TargetTable(drop.name_path(), scope);
  if (!path) {
    return std::nullopt;
  }
  return "DROP VIEW " + std::string(drop.is_if_exists() ? "IF EXISTS " : "") + *path;
}

// A table snapshot is a DuckDB table; DROP SNAPSHOT TABLE drops only snapshots, which the
// emulator checks when it runs.
std::optional<std::string> DropSnapshotTable(const googlesql::ResolvedDropSnapshotTableStmt& drop,
                                             const Scope& scope) {
  const auto path = TargetTable(drop.name_path(), scope);
  if (!path) {
    return std::nullopt;
  }
  return "DROP TABLE " + std::string(drop.is_if_exists() ? "IF EXISTS " : "") + *path;
}

}  // namespace bigquery_emulator_duckdb::translator
