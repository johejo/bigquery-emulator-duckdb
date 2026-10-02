#include "src/ddl_write.h"

#include <algorithm>
#include <format>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

#include "absl/strings/str_join.h"
#include "src/column_metadata.h"
#include "src/duckdb_sql.h"
#include "src/references.h"
#include "src/table_comments.h"
#include "src/translator.h"

namespace bigquery_emulator_duckdb {
namespace {

std::string TableExists(const TableReference& table) {
  return std::format(
      "EXISTS (SELECT 1 FROM information_schema.tables"
      " WHERE table_catalog = {} AND table_schema = {} AND table_name = {})",
      QuoteLiteral(table.project_id), QuoteLiteral(table.dataset_id), QuoteLiteral(table.table_id));
}

DdlWrite AddColumnWrite(const AddedColumn& column) {
  DdlWrite write{.metadata_statements = ColumnCommentStatements(column.table, {column.field})};
  std::vector<std::string> skip;
  if (column.if_table_exists) {
    skip.push_back("NOT " + TableExists(column.table));
  }
  if (column.if_column_not_exists) {
    skip.push_back(std::format(
        "EXISTS (SELECT 1 FROM duckdb_columns() WHERE database_name = {} AND schema_name = {}"
        " AND table_name = {} AND lower(column_name) = {})",
        QuoteLiteral(column.table.project_id), QuoteLiteral(column.table.dataset_id),
        QuoteLiteral(column.table.table_id), QuoteLiteral(ToLowerAscii(column.field.name))));
  }
  if (!skip.empty()) {
    write.skip_query = "SELECT 1 WHERE " + absl::StrJoin(skip, " OR ");
  }
  return write;
}

}  // namespace

DdlWrite CreateViewWrite(const ViewDefinition& view) {
  const TableReference& table = view.table;
  DdlWrite write;
  // DuckDB can create a circular view and only reject it when queried. Bind the new definition
  // before committing so a failed replacement keeps the old view.
  write.metadata_statements = {
      "SELECT * FROM " + QualifiedName(table) + " LIMIT 0",
      ViewCommentStatement(table, {view.query, view.schema, view.metadata})};
  if (view.if_not_exists) {
    write.skip_query = "SELECT 1 WHERE " + TableExists(table);
  }
  return write;
}

DdlWrite CreateTableWrite(const TableDefinition& definition) {
  DdlWrite write{.metadata_statements =
                     ColumnCommentStatements(definition.table, definition.schema)};
  std::ranges::move(RepeatedColumnDefaultStatements(definition.table, definition.schema),
                    std::back_inserter(write.metadata_statements));
  if (!definition.metadata.empty()) {
    write.metadata_statements.push_back(
        TableCommentStatement(definition.table, definition.metadata, definition.schema));
  }
  if (definition.if_not_exists) {
    write.skip_query = "SELECT 1 WHERE " + TableExists(definition.table);
  }
  return write;
}

std::optional<DdlWrite> MetadataWrite(const TranslatedStatement& statement) {
  if (statement.table.has_value()) {
    return CreateTableWrite(*statement.table);
  }
  if (statement.added_column.has_value()) {
    return AddColumnWrite(*statement.added_column);
  }
  if (statement.view.has_value()) {
    return CreateViewWrite(*statement.view);
  }
  return std::nullopt;
}

}  // namespace bigquery_emulator_duckdb
