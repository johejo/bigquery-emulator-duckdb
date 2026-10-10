#include "src/translator/statement.h"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "googlesql/public/type.h"
#include "googlesql/resolved_ast/resolved_ast.h"
#include "src/analyzer.h"
#include "src/duckdb_sql.h"
#include "src/translator/context.h"
#include "src/translator/ddl.h"
#include "src/translator/dml.h"
#include "src/translator/scan.h"

namespace bigquery_emulator_duckdb::translator {
std::optional<std::string> Statement(const googlesql::ResolvedStatement& statement,
                                     const Scope& scope) {
  if (!statement.hint_list().empty()) {
    return Unsupported(scope, "statement hints");
  }
  if (statement.Is<googlesql::ResolvedInsertStmt>()) {
    return Insert(*statement.GetAs<googlesql::ResolvedInsertStmt>(), scope);
  }
  if (statement.Is<googlesql::ResolvedUpdateStmt>()) {
    return Update(*statement.GetAs<googlesql::ResolvedUpdateStmt>(), scope);
  }
  if (statement.Is<googlesql::ResolvedDeleteStmt>()) {
    return Delete(*statement.GetAs<googlesql::ResolvedDeleteStmt>(), scope);
  }
  if (statement.Is<googlesql::ResolvedTruncateStmt>()) {
    return Truncate(*statement.GetAs<googlesql::ResolvedTruncateStmt>(), scope);
  }
  if (statement.Is<googlesql::ResolvedMergeStmt>()) {
    return Merge(*statement.GetAs<googlesql::ResolvedMergeStmt>(), scope);
  }
  if (statement.Is<googlesql::ResolvedCreateTableStmt>()) {
    return CreateTable(*statement.GetAs<googlesql::ResolvedCreateTableStmt>(), scope);
  }
  if (statement.Is<googlesql::ResolvedAlterTableStmt>()) {
    return AlterTable(*statement.GetAs<googlesql::ResolvedAlterTableStmt>(), scope);
  }
  if (statement.Is<googlesql::ResolvedAlterTableSetOptionsStmt>()) {
    return AlterTableSetOptions(*statement.GetAs<googlesql::ResolvedAlterTableSetOptionsStmt>(),
                                scope);
  }
  if (statement.Is<googlesql::ResolvedCreateTableAsSelectStmt>()) {
    return CreateTableAsSelect(*statement.GetAs<googlesql::ResolvedCreateTableAsSelectStmt>(),
                               scope);
  }
  if (statement.Is<googlesql::ResolvedCreateViewStmt>()) {
    return CreateView(*statement.GetAs<googlesql::ResolvedCreateViewStmt>(), scope);
  }
  if (statement.Is<googlesql::ResolvedCreateSchemaStmt>()) {
    return CreateSchema(*statement.GetAs<googlesql::ResolvedCreateSchemaStmt>(), scope);
  }
  if (statement.Is<googlesql::ResolvedAlterSchemaStmt>()) {
    return AlterSchema(*statement.GetAs<googlesql::ResolvedAlterSchemaStmt>(), scope);
  }
  if (statement.Is<googlesql::ResolvedDropStmt>()) {
    return Drop(*statement.GetAs<googlesql::ResolvedDropStmt>(), scope);
  }
  if (statement.Is<googlesql::ResolvedCreateFunctionStmt>()) {
    return CreateFunction(*statement.GetAs<googlesql::ResolvedCreateFunctionStmt>(), scope);
  }
  if (statement.Is<googlesql::ResolvedDropFunctionStmt>()) {
    return DropFunction(*statement.GetAs<googlesql::ResolvedDropFunctionStmt>(), scope);
  }
  if (!statement.Is<googlesql::ResolvedQueryStmt>()) {
    return Unsupported(scope, "statement " + statement.node_kind_string());
  }
  const auto* query = statement.GetAs<googlesql::ResolvedQueryStmt>();
  const auto relation = Scan(*query->query(), scope);
  if (!relation) {
    return std::nullopt;
  }
  std::vector<std::string> projections;
  // A value table of structs is returned as the structs' fields, like BigQuery does.
  if (query->is_value_table() && query->output_column_list_size() == 1 &&
      query->output_column_list(0)->column().type()->IsStruct()) {
    const auto& output = query->output_column_list(0)->column();
    const auto column = relation->columns.find(output.column_id());
    const auto& fields = output.type()->AsStruct()->fields();
    if (column == relation->columns.end() || fields.empty()) {
      return Unsupported(scope, "SELECT AS STRUCT without fields");
    }
    for (size_t i = 0; i < fields.size(); ++i) {
      projections.push_back(
          "struct_extract_at(" + column->second + ", " + std::to_string(i + 1) + ") AS " +
          QuoteIdentifier(ValueTableFieldName(fields.at(i).name, static_cast<int>(i))));
    }
    return "SELECT " + Join(projections, ", ") + relation->From() + relation->Order();
  }
  for (const auto& output : query->output_column_list()) {
    const auto column = relation->columns.find(output->column().column_id());
    if (column == relation->columns.end()) {
      return std::nullopt;
    }
    projections.push_back(column->second + " AS " + QuoteIdentifier(output->name()));
  }
  if (projections.empty()) {
    return std::nullopt;
  }
  return "SELECT " + Join(projections, ", ") + relation->From() + relation->Order();
}

// JobStatistics2.statementType of a statement the translator supports.
std::string StatementType(const googlesql::ResolvedStatement& statement) {
  if (statement.Is<googlesql::ResolvedInsertStmt>()) {
    return "INSERT";
  }
  if (statement.Is<googlesql::ResolvedUpdateStmt>()) {
    return "UPDATE";
  }
  if (statement.Is<googlesql::ResolvedDeleteStmt>()) {
    return "DELETE";
  }
  if (statement.Is<googlesql::ResolvedTruncateStmt>()) {
    return "TRUNCATE_TABLE";
  }
  if (statement.Is<googlesql::ResolvedMergeStmt>()) {
    return "MERGE";
  }
  if (statement.Is<googlesql::ResolvedCreateTableStmt>()) {
    return "CREATE_TABLE";
  }
  if (statement.Is<googlesql::ResolvedCreateTableAsSelectStmt>()) {
    return "CREATE_TABLE_AS_SELECT";
  }
  if (statement.Is<googlesql::ResolvedAlterTableStmt>() ||
      statement.Is<googlesql::ResolvedAlterTableSetOptionsStmt>()) {
    return "ALTER_TABLE";
  }
  if (statement.Is<googlesql::ResolvedAlterSchemaStmt>()) {
    return "ALTER_SCHEMA";
  }
  if (statement.Is<googlesql::ResolvedCreateViewStmt>()) {
    return "CREATE_VIEW";
  }
  if (statement.Is<googlesql::ResolvedCreateSchemaStmt>()) {
    return "CREATE_SCHEMA";
  }
  if (statement.Is<googlesql::ResolvedDropStmt>()) {
    return "DROP_" + ToUpperAscii(statement.GetAs<googlesql::ResolvedDropStmt>()->object_type());
  }
  if (statement.Is<googlesql::ResolvedCreateFunctionStmt>()) {
    return "CREATE_FUNCTION";
  }
  if (statement.Is<googlesql::ResolvedDropFunctionStmt>()) {
    return "DROP_FUNCTION";
  }
  return "SELECT";
}

}  // namespace bigquery_emulator_duckdb::translator
