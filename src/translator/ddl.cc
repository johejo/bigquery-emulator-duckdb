#include <optional>
#include <string>
#include <vector>

#include "googlesql/resolved_ast/resolved_ast.h"
#include "src/catalog.h"
#include "src/duckdb_sql.h"
#include "src/translator/internal.h"

namespace bigquery_emulator_duckdb::translator {

namespace {

// DDL names tables and datasets by path rather than through the catalog, so the defaults the
// catalog resolves queries with are applied here.
std::optional<std::string> TablePath(const std::vector<std::string>& path,
                                     const DefaultDataset& defaults, const Scope& scope) {
  const auto parts = NormalizeTablePath(path, defaults.project, defaults.dataset);
  if (parts.empty()) {
    return Unsupported(scope, "table name " + Join(path, "."));
  }
  std::vector<std::string> quoted;
  quoted.reserve(parts.size());
  for (const auto& part : parts) {
    quoted.push_back(QuoteIdentifier(part));
  }
  return Join(quoted, ".");
}

std::optional<std::string> DatasetPath(const std::vector<std::string>& path,
                                       const DefaultDataset& defaults, const Scope& scope) {
  // Reuse table path normalization so dots inside a domain-scoped project are handled
  // the same way for CREATE/DROP SCHEMA and table references.
  std::vector<std::string> table_path = path;
  table_path.push_back("_dataset_path_placeholder");
  const auto parts = NormalizeTablePath(table_path, defaults.project, defaults.dataset);
  if (path.empty() || parts.empty()) {
    return Unsupported(scope, "dataset name " + Join(path, "."));
  }
  return QuoteIdentifier(parts[0]) + "." + QuoteIdentifier(parts[1]);
}

bool HasCollation(const googlesql::ResolvedColumnAnnotations* annotations) {
  if (annotations == nullptr) {
    return false;
  }
  if (annotations->collation_name() != nullptr) {
    return true;
  }
  for (const auto& child : annotations->child_list()) {
    if (HasCollation(child.get())) {
      return true;
    }
  }
  return false;
}

std::optional<std::string> ColumnDefinitionType(const googlesql::ResolvedColumnDefinition& column,
                                                const Scope& scope) {
  if (HasCollation(column.annotations())) {
    return Unsupported(scope, "column collation");
  }
  auto type =
      SqlType(column.type(),
              column.annotations() == nullptr ? nullptr : &column.annotations()->type_parameters());
  if (!type) {
    return Unsupported(scope, "column type " + column.type()->DebugString());
  }
  return type;
}

// CREATE [OR REPLACE] TABLE [IF NOT EXISTS] path, shared with CREATE TABLE AS SELECT.
// Partitioning, clustering and options only shape BigQuery storage, and BigQuery's primary and
// foreign keys are never enforced, so they are all dropped.
std::optional<std::string> CreateTableHead(const googlesql::ResolvedCreateTableStmtBase& create,
                                           const DefaultDataset& defaults, const Scope& scope) {
  if (create.create_scope() == googlesql::ResolvedCreateStatement::CREATE_TEMP) {
    return Unsupported(scope, "temporary tables");
  }
  if (create.like_table() != nullptr) {
    return Unsupported(scope, "CREATE TABLE LIKE");
  }
  if (create.is_value_table() || !create.pseudo_column_list().empty() ||
      create.collation_name() != nullptr || create.connection() != nullptr ||
      !create.check_constraint_list().empty()) {
    return Unsupported(scope, "CREATE TABLE option");
  }
  const auto path = TablePath(create.name_path(), defaults, scope);
  if (!path) {
    return std::nullopt;
  }
  switch (create.create_mode()) {
    case googlesql::ResolvedCreateStatement::CREATE_OR_REPLACE:
      return "CREATE OR REPLACE TABLE " + *path;
    case googlesql::ResolvedCreateStatement::CREATE_IF_NOT_EXISTS:
      return "CREATE TABLE IF NOT EXISTS " + *path;
    default:
      return "CREATE TABLE " + *path;
  }
}

}  // namespace

std::optional<std::string> AlterTable(const googlesql::ResolvedAlterTableStmt& alter,
                                      const DefaultDataset& defaults, const Scope& scope) {
  // DuckDB accepts only one action per ALTER. Reject the whole statement rather than
  // applying only some of its actions.
  if (alter.alter_action_list_size() != 1) {
    return Unsupported(scope, "ALTER TABLE with multiple actions");
  }
  const auto& action = *alter.alter_action_list(0);
  if (!action.Is<googlesql::ResolvedAddColumnAction>()) {
    return Unsupported(scope, "ALTER TABLE action " + action.node_kind_string());
  }
  const auto& add = *action.GetAs<googlesql::ResolvedAddColumnAction>();
  const auto& column = *add.column_definition();
  if (column.is_hidden() || column.generated_column_info() != nullptr ||
      column.default_value() != nullptr ||
      (column.annotations() != nullptr && column.annotations()->not_null())) {
    return Unsupported(scope, "ADD COLUMN with generated columns, defaults or NOT NULL");
  }
  const auto path = TablePath(alter.name_path(), defaults, scope);
  const auto type = ColumnDefinitionType(column, scope);
  if (!path || !type) {
    return std::nullopt;
  }
  return std::string("ALTER TABLE ") + (alter.is_if_exists() ? "IF EXISTS " : "") + *path +
         " ADD COLUMN " + (add.is_if_not_exists() ? "IF NOT EXISTS " : "") +
         QuoteIdentifier(column.name()) + " " + *type;
}

std::optional<std::string> CreateTable(const googlesql::ResolvedCreateTableStmt& create,
                                       const DefaultDataset& defaults, const Scope& scope) {
  if (create.clone_from() != nullptr || create.copy_from() != nullptr) {
    return Unsupported(scope, "CREATE TABLE CLONE or COPY");
  }
  const auto head = CreateTableHead(create, defaults, scope);
  if (!head) {
    return std::nullopt;
  }
  std::vector<std::string> columns;
  for (const auto& column : create.column_definition_list()) {
    if (column->is_hidden() || column->generated_column_info() != nullptr) {
      return Unsupported(scope, "generated columns");
    }
    const auto type = ColumnDefinitionType(*column, scope);
    if (!type) {
      return std::nullopt;
    }
    std::string sql = QuoteIdentifier(column->name()) + " " + *type;
    if (column->annotations() != nullptr && column->annotations()->not_null()) {
      sql += " NOT NULL";
    }
    if (column->default_value() != nullptr) {
      // A parameterized column's default is cast to its type with the parameters, which the
      // column itself applies when the default is stored.
      const googlesql::ResolvedExpr* expression = column->default_value()->expression();
      if (expression->Is<googlesql::ResolvedCast>()) {
        const auto* cast = expression->GetAs<googlesql::ResolvedCast>();
        if (cast->format() == nullptr && cast->time_zone() == nullptr &&
            cast->extended_cast() == nullptr && !cast->return_null_on_error()) {
          expression = cast->expr();
        }
      }
      const auto value = Expression(*expression, scope, {});
      if (!value) {
        return std::nullopt;
      }
      sql += " DEFAULT " + *value;
    }
    columns.push_back(sql);
  }
  if (columns.empty()) {
    return Unsupported(scope, "CREATE TABLE without columns");
  }
  return *head + " (" + Join(columns, ", ") + ")";
}

// The query's columns are cast to the declared types, which DuckDB would otherwise infer.
std::optional<std::string> CreateTableAsSelect(
    const googlesql::ResolvedCreateTableAsSelectStmt& create, const DefaultDataset& defaults,
    const Scope& scope) {
  if (create.output_column_list_size() != create.column_definition_list_size()) {
    return Unsupported(scope, "CREATE TABLE AS SELECT columns");
  }
  const auto head = CreateTableHead(create, defaults, scope);
  if (!head) {
    return std::nullopt;
  }
  const auto relation = Scan(*create.query(), scope);
  if (!relation) {
    return std::nullopt;
  }
  std::vector<std::string> projections;
  for (int i = 0; i < create.output_column_list_size(); ++i) {
    const auto& definition = *create.column_definition_list(i);
    // DuckDB cannot declare constraints on a table created from a query.
    if (definition.annotations() != nullptr && definition.annotations()->not_null()) {
      return Unsupported(scope, "NOT NULL in CREATE TABLE AS SELECT");
    }
    const auto column = relation->columns.find(create.output_column_list(i)->column().column_id());
    const auto type = ColumnDefinitionType(definition, scope);
    if (column == relation->columns.end() || !type) {
      return std::nullopt;
    }
    projections.push_back("CAST(" + column->second + " AS " + *type + ") AS " +
                          QuoteIdentifier(definition.name()));
  }
  return *head + " AS SELECT " + Join(projections, ", ") + relation->From() + relation->Order();
}

std::optional<std::string> CreateView(const googlesql::ResolvedCreateViewStmt& create,
                                      const DefaultDataset& defaults, const Scope& scope) {
  if (create.create_scope() != googlesql::ResolvedCreateStatement::CREATE_DEFAULT_SCOPE ||
      create.recursive() || create.is_value_table()) {
    return Unsupported(scope, "temporary, recursive or value-table views");
  }
  const auto path = TablePath(create.name_path(), defaults, scope);
  const auto relation = Scan(*create.query(), scope);
  if (!path || !relation) {
    return std::nullopt;
  }
  std::vector<std::string> projections;
  for (const auto& output : create.output_column_list()) {
    const auto column = relation->columns.find(output->column().column_id());
    const auto type = SqlType(output->column().type());
    if (column == relation->columns.end() || !type) {
      return std::nullopt;
    }
    // Preserve GoogleSQL result types when the view is later read through the catalog.
    projections.push_back("CAST(" + column->second + " AS " + *type + ") AS " +
                          QuoteIdentifier(output->name()));
  }
  std::string head = "CREATE ";
  if (create.create_mode() == googlesql::ResolvedCreateStatement::CREATE_OR_REPLACE) {
    head += "OR REPLACE ";
  }
  head += "VIEW ";
  if (create.create_mode() == googlesql::ResolvedCreateStatement::CREATE_IF_NOT_EXISTS) {
    head += "IF NOT EXISTS ";
  }
  return head + *path + " AS SELECT " + Join(projections, ", ") + relation->From() +
         relation->Order();
}

std::optional<std::string> CreateSchema(const googlesql::ResolvedCreateSchemaStmt& create,
                                        const DefaultDataset& defaults, const Scope& scope) {
  if (create.collation_name() != nullptr) {
    return Unsupported(scope, "dataset collation");
  }
  const auto path = DatasetPath(create.name_path(), defaults, scope);
  if (!path) {
    return std::nullopt;
  }
  switch (create.create_mode()) {
    case googlesql::ResolvedCreateStatement::CREATE_OR_REPLACE:
      return Unsupported(scope, "CREATE OR REPLACE SCHEMA");
    case googlesql::ResolvedCreateStatement::CREATE_IF_NOT_EXISTS:
      return "CREATE SCHEMA IF NOT EXISTS " + *path;
    default:
      return "CREATE SCHEMA " + *path;
  }
}

std::optional<std::string> Drop(const googlesql::ResolvedDropStmt& drop,
                                const DefaultDataset& defaults, const Scope& scope) {
  const std::string object_type = ToUpperAscii(drop.object_type());
  const bool is_schema = object_type == "SCHEMA";
  if (object_type != "TABLE" && object_type != "VIEW" && !is_schema) {
    return Unsupported(scope, "DROP " + object_type);
  }
  const auto path = is_schema ? DatasetPath(drop.name_path(), defaults, scope)
                              : TablePath(drop.name_path(), defaults, scope);
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
