#include "src/translator/ddl.h"

#include <optional>
#include <string>

#include "googlesql/resolved_ast/resolved_ast.h"
#include "src/duckdb_sql.h"
#include "src/translator/context.h"
#include "src/translator/ddl_names.h"

namespace bigquery_emulator_duckdb::translator {

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
