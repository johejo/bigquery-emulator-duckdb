#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "googlesql/public/catalog.h"
#include "googlesql/public/type.h"
#include "googlesql/resolved_ast/resolved_ast.h"
#include "src/catalog.h"
#include "src/duckdb_sql.h"
#include "src/translator/internal.h"

namespace bigquery_emulator_duckdb::translator {

namespace {

// The table modified by a DML statement, aliased as _t.
struct Target {
  std::string table;
  // Resolved column ID to the table's own column name.
  std::map<int, std::string> names;
  // The same columns qualified by the alias, for expressions.
  Columns columns;
  std::map<int, const googlesql::Type*> column_types;
};

std::optional<Target> DmlTarget(const googlesql::ResolvedTableScan& table, const Scope& scope) {
  if (!table.hint_list().empty() || table.for_system_time_expr() != nullptr ||
      table.lock_mode() != nullptr || table.table()->IsValueTable() ||
      table.column_list_size() != table.column_index_list_size()) {
    return Unsupported(scope, "DML target with hints, FOR SYSTEM_TIME or a value table");
  }
  if (dynamic_cast<const SqlTable*>(table.table()) != nullptr) {
    return Unsupported(scope, "DML on an INFORMATION_SCHEMA view");
  }
  Target target{QuoteIdentifierPath(table.table()->FullName()), {}, {}, {}};
  for (int i = 0; i < table.column_list_size(); ++i) {
    const auto& column = table.column_list(i);
    if (!SqlType(column.type()) || column.type_annotation_map() != nullptr) {
      return Unsupported(scope, "column type " + column.type()->DebugString());
    }
    const std::string name =
        QuoteIdentifier(table.table()->GetColumn(table.column_index_list(i))->Name());
    target.names.emplace(column.column_id(), name);
    target.columns.emplace(column.column_id(), "_t." + name);
    target.column_types.emplace(column.column_id(), column.type());
  }
  return target;
}

// Correlated subqueries see the target row through the outer columns.
Scope DmlScope(const Scope& scope, const Target& target) {
  Scope dml = scope;
  dml.outer.insert(target.columns.begin(), target.columns.end());
  return dml;
}

std::optional<std::string> DmlValue(const googlesql::ResolvedDMLValue& value, const Scope& scope,
                                    const Columns& columns) {
  if (value.value()->Is<googlesql::ResolvedDMLDefault>()) {
    return "DEFAULT";
  }
  return Expression(*value.value(), scope, columns);
}

// The assignments below one column or struct field: either its new value, or the fields of it
// that are assigned, by field index.
struct UpdateNode {
  std::optional<std::string> value;
  std::map<int, UpdateNode> fields;
};

// The new value of a struct `original` of `type` whose fields `node` assigns: the struct rebuilt
// from its assigned and original fields. Setting a field of a NULL struct is an error.
std::optional<std::string> UpdatedStruct(const std::string& original, const googlesql::Type* type,
                                         const UpdateNode& node) {
  if (node.value) {
    return node.value;
  }
  const auto sql_type = SqlType(type);
  if (!sql_type) {
    return std::nullopt;
  }
  std::vector<std::string> fields;
  for (int i = 0; i < type->AsStruct()->num_fields(); ++i) {
    const auto& field = type->AsStruct()->field(i);
    std::string value = "struct_extract_at(" + original + ", " + std::to_string(i + 1) + ")";
    if (const auto child = node.fields.find(i); child != node.fields.end()) {
      const auto sql = UpdatedStruct(value, field.type, child->second);
      if (!sql) {
        return std::nullopt;
      }
      value = *sql;
    }
    fields.push_back(QuoteIdentifier(field.name) + " := " + value);
  }
  return "CASE WHEN " + original + " IS NULL THEN error(" +
         QuoteLiteral("Cannot set field of NULL " + type->TypeName(googlesql::PRODUCT_EXTERNAL)) +
         ") ELSE CAST(struct_pack(" + Join(fields, ", ") + ") AS " + *sql_type + ") END";
}

// SET assignments of whole columns and struct fields. Array elements and nested DML are not.
std::optional<std::string> UpdateItems(
    const std::vector<std::unique_ptr<const googlesql::ResolvedUpdateItem>>& items,
    const Target& target, const Scope& scope, const Columns& columns) {
  // Assignments by column ID, in the order the columns are first assigned.
  std::vector<int> order;
  std::map<int, UpdateNode> nodes;
  for (const auto& item : items) {
    if (item->set_value() == nullptr || item->element_column() != nullptr ||
        !item->update_item_element_list().empty() || !item->delete_list().empty() ||
        !item->update_list().empty() || !item->insert_list().empty()) {
      return Unsupported(scope, "UPDATE of array elements or nested DML");
    }
    std::vector<int> path;
    const googlesql::ResolvedExpr* target_expr = item->target();
    while (target_expr->Is<googlesql::ResolvedGetStructField>()) {
      const auto* get = target_expr->GetAs<googlesql::ResolvedGetStructField>();
      path.insert(path.begin(), get->field_idx());
      target_expr = get->expr();
    }
    if (!target_expr->Is<googlesql::ResolvedColumnRef>()) {
      return Unsupported(scope, "UPDATE target");
    }
    const int id = target_expr->GetAs<googlesql::ResolvedColumnRef>()->column().column_id();
    if (!target.names.contains(id) || !target.columns.contains(id)) {
      return Unsupported(scope, "UPDATE target");
    }
    const auto value = DmlValue(*item->set_value(), scope, columns);
    if (!value) {
      return std::nullopt;
    }
    if (!nodes.contains(id)) {
      order.push_back(id);
    }
    UpdateNode* node = &nodes[id];
    for (const int field : path) {
      node = &node->fields[field];
    }
    node->value = *value;
  }
  std::vector<std::string> assignments;
  for (const int id : order) {
    const auto value =
        UpdatedStruct(target.columns.at(id), target.column_types.at(id), nodes.at(id));
    if (!value) {
      return std::nullopt;
    }
    assignments.push_back(target.names.at(id) + " = " + *value);
  }
  if (assignments.empty()) {
    return Unsupported(scope, "UPDATE without assignments");
  }
  return Join(assignments, ", ");
}

// Materialize the source once so the cardinality check and the write see the same
// rows, including when the source contains volatile expressions. The scalar guard
// raises inside the DML statement, so DuckDB rolls back the entire write.
std::string CheckedDmlSource(const std::string& source, const Target& target,
                             const std::string& condition, const std::string& affected = "TRUE") {
  return "WITH _bq_source AS MATERIALIZED (" + source +
         ") SELECT * FROM _bq_source WHERE (SELECT CASE WHEN EXISTS (SELECT 1 FROM " +
         target.table + " AS _t WHERE (SELECT count(*) FROM _bq_source AS q WHERE " + condition +
         ") > 1 AND EXISTS (SELECT 1 FROM _bq_source AS q WHERE (" + condition + ") AND (" +
         affected +
         "))) THEN error('UPDATE/MERGE must match at most one source row for each target row') "
         "ELSE TRUE END)";
}

}  // namespace

std::optional<std::string> Insert(const googlesql::ResolvedInsertStmt& insert, const Scope& scope) {
  const auto* table = insert.table_scan();
  if (insert.insert_mode() != googlesql::ResolvedInsertStmt::OR_ERROR ||
      insert.assert_rows_modified() != nullptr || insert.returning() != nullptr ||
      insert.on_conflict_clause() != nullptr || insert.query_parameter_list_size() != 0 ||
      insert.generated_column_expr_list_size() != 0 ||
      insert.timestamp_version_column() != nullptr || insert.temporal_at() != nullptr ||
      !table->hint_list().empty() || table->for_system_time_expr() != nullptr ||
      table->table()->IsValueTable() ||
      table->column_list_size() != table->column_index_list_size()) {
    return Unsupported(scope,
                       "INSERT with OR IGNORE/REPLACE/UPDATE, ASSERT_ROWS_MODIFIED, THEN RETURN, "
                       "ON CONFLICT or generated columns");
  }
  if (dynamic_cast<const SqlTable*>(table->table()) != nullptr) {
    return Unsupported(scope, "DML on an INFORMATION_SCHEMA view");
  }
  // The inserted columns are the table scan's columns; name them by the table's own columns.
  std::map<int, std::string> table_columns;
  for (int i = 0; i < table->column_list_size(); ++i) {
    table_columns.emplace(
        table->column_list(i).column_id(),
        QuoteIdentifier(table->table()->GetColumn(table->column_index_list(i))->Name()));
  }
  std::vector<std::string> names;
  for (const auto& column : insert.insert_column_list()) {
    const auto name = table_columns.find(column.column_id());
    if (name == table_columns.end() || !SqlType(column.type()) ||
        column.type_annotation_map() != nullptr) {
      return std::nullopt;
    }
    names.push_back(name->second);
  }
  std::string sql = "INSERT INTO " + QuoteIdentifierPath(table->table()->FullName()) + " (" +
                    Join(names, ", ") + ") ";
  if (insert.query() != nullptr) {
    const auto relation = Scan(*insert.query(), scope);
    if (!relation) {
      return std::nullopt;
    }
    std::vector<std::string> projections;
    for (const auto& output : insert.query_output_column_list()) {
      const auto column = relation->columns.find(output.column_id());
      if (column == relation->columns.end()) {
        return std::nullopt;
      }
      projections.push_back(column->second);
    }
    return sql + "SELECT " + Join(projections, ", ") + relation->From();
  }
  if (insert.row_list_size() == 0) {
    return std::nullopt;
  }
  std::vector<std::string> rows;
  for (const auto& row : insert.row_list()) {
    std::vector<std::string> values;
    for (const auto& dml_value : row->value_list()) {
      const auto* value = dml_value->value();
      if (value->Is<googlesql::ResolvedDMLDefault>()) {
        values.emplace_back("DEFAULT");
        continue;
      }
      const auto sql_value = Expression(*value, scope, {});
      if (!sql_value) {
        return std::nullopt;
      }
      values.push_back(*sql_value);
    }
    rows.push_back("(" + Join(values, ", ") + ")");
  }
  return sql + "VALUES " + Join(rows, ", ");
}

std::optional<std::string> Update(const googlesql::ResolvedUpdateStmt& update, const Scope& scope) {
  if (update.assert_rows_modified() != nullptr || update.returning() != nullptr ||
      update.array_offset_column() != nullptr || update.generated_column_expr_list_size() != 0 ||
      update.timestamp_version_column() != nullptr || update.temporal_at() != nullptr) {
    return Unsupported(scope, "UPDATE with ASSERT_ROWS_MODIFIED, THEN RETURN or generated columns");
  }
  const auto target = DmlTarget(*update.table_scan(), scope);
  if (!target) {
    return std::nullopt;
  }
  const Scope dml = DmlScope(scope, *target);
  Columns columns = target->columns;
  std::string from;
  std::string source_sql;
  if (update.from_scan() != nullptr) {
    const auto relation = Scan(*update.from_scan(), dml);
    if (!relation) {
      return std::nullopt;
    }
    columns.insert(relation->columns.begin(), relation->columns.end());
    from = relation->From();
    source_sql = relation->sql;
  }
  const auto assignments = UpdateItems(update.update_item_list(), *target, dml, columns);
  if (!assignments) {
    return std::nullopt;
  }
  std::string condition = "TRUE";
  if (update.where_expr() != nullptr) {
    const auto where = Expression(*update.where_expr(), dml, columns);
    if (!where) {
      return std::nullopt;
    }
    condition = *where;
  }
  if (update.from_scan() != nullptr) {
    from = " FROM (" + CheckedDmlSource(source_sql, *target, condition) + ") AS q";
  }
  std::string sql =
      "UPDATE " + target->table + " AS _t SET " + *assignments + from + " WHERE " + condition;
  return sql;
}

std::optional<std::string> Delete(const googlesql::ResolvedDeleteStmt& del, const Scope& scope) {
  if (del.assert_rows_modified() != nullptr || del.returning() != nullptr ||
      del.array_offset_column() != nullptr || del.timestamp_version_column() != nullptr ||
      del.using_scan() != nullptr) {
    return Unsupported(scope, "DELETE with ASSERT_ROWS_MODIFIED, THEN RETURN or USING");
  }
  const auto target = DmlTarget(*del.table_scan(), scope);
  if (!target) {
    return std::nullopt;
  }
  std::string sql = "DELETE FROM " + target->table + " AS _t";
  if (del.where_expr() != nullptr) {
    const auto where = Expression(*del.where_expr(), DmlScope(scope, *target), target->columns);
    if (!where) {
      return std::nullopt;
    }
    sql += " WHERE " + *where;
  }
  return sql;
}

std::optional<std::string> Truncate(const googlesql::ResolvedTruncateStmt& truncate,
                                    const Scope& scope) {
  // Partition filters cannot be preserved: partitioning is not represented in DuckDB.
  if (truncate.where_expr() != nullptr) {
    return Unsupported(scope, "TRUNCATE TABLE with WHERE");
  }
  const auto target = DmlTarget(*truncate.table_scan(), scope);
  if (!target) {
    return std::nullopt;
  }
  return "TRUNCATE TABLE " + target->table;
}

std::optional<std::string> MergeClause(const googlesql::ResolvedMergeWhen& when,
                                       const Target& target, const Scope& scope,
                                       const Columns& columns) {
  std::string sql;
  switch (when.match_type()) {
    case googlesql::ResolvedMergeWhen::MATCHED:
      sql = "WHEN MATCHED";
      break;
    case googlesql::ResolvedMergeWhen::NOT_MATCHED_BY_TARGET:
      sql = "WHEN NOT MATCHED BY TARGET";
      break;
    case googlesql::ResolvedMergeWhen::NOT_MATCHED_BY_SOURCE:
      sql = "WHEN NOT MATCHED BY SOURCE";
      break;
    default:
      return Unsupported(scope, "MERGE match type");
  }
  if (when.match_expr() != nullptr) {
    const auto condition = Expression(*when.match_expr(), scope, columns);
    if (!condition) {
      return std::nullopt;
    }
    sql += " AND " + *condition;
  }
  switch (when.action_type()) {
    case googlesql::ResolvedMergeWhen::DELETE:
      return sql + " THEN DELETE";
    case googlesql::ResolvedMergeWhen::UPDATE: {
      const auto assignments = UpdateItems(when.update_item_list(), target, scope, columns);
      if (!assignments) {
        return std::nullopt;
      }
      return sql + " THEN UPDATE SET " + *assignments;
    }
    case googlesql::ResolvedMergeWhen::INSERT: {
      if (when.insert_row() == nullptr ||
          when.insert_row()->value_list_size() != when.insert_column_list_size()) {
        return Unsupported(scope, "MERGE INSERT");
      }
      std::vector<std::string> names;
      std::vector<std::string> values;
      for (int i = 0; i < when.insert_column_list_size(); ++i) {
        const auto name = target.names.find(when.insert_column_list(i).column_id());
        if (name == target.names.end()) {
          return Unsupported(scope, "MERGE INSERT column");
        }
        const auto value = DmlValue(*when.insert_row()->value_list(i), scope, columns);
        if (!value) {
          return std::nullopt;
        }
        names.push_back(name->second);
        values.push_back(*value);
      }
      return sql + " THEN INSERT (" + Join(names, ", ") + ") VALUES (" + Join(values, ", ") + ")";
    }
    default:
      return Unsupported(scope, "MERGE action");
  }
}

std::optional<std::string> Merge(const googlesql::ResolvedMergeStmt& merge, const Scope& scope) {
  const auto target = DmlTarget(*merge.table_scan(), scope);
  if (!target) {
    return std::nullopt;
  }
  const Scope dml = DmlScope(scope, *target);
  const auto source = Scan(*merge.from_scan(), dml);
  if (!source) {
    return std::nullopt;
  }
  Columns columns = target->columns;
  columns.insert(source->columns.begin(), source->columns.end());
  const auto condition = Expression(*merge.merge_expr(), dml, columns);
  if (!condition) {
    return std::nullopt;
  }
  std::vector<std::string> clauses;
  for (const auto& when : merge.when_clause_list()) {
    const auto clause = MergeClause(*when, *target, dml, columns);
    if (!clause) {
      return std::nullopt;
    }
    clauses.push_back(*clause);
  }
  std::vector<std::string> affected;
  bool has_matched_update = false;
  for (const auto& when : merge.when_clause_list()) {
    if (when->match_type() != googlesql::ResolvedMergeWhen::MATCHED) {
      continue;
    }
    has_matched_update |= when->action_type() == googlesql::ResolvedMergeWhen::UPDATE;
    const auto predicate = when->match_expr() == nullptr
                               ? std::optional<std::string>("TRUE")
                               : Expression(*when->match_expr(), dml, columns);
    if (!predicate) {
      return std::nullopt;
    }
    affected.push_back("(" + *predicate + ")");
  }
  const std::string source_sql =
      has_matched_update
          ? CheckedDmlSource(source->sql, *target, *condition, Join(affected, " OR "))
          : source->sql;
  return "MERGE INTO " + target->table + " AS _t USING (" + source_sql + ") AS q ON " + *condition +
         " " + Join(clauses, " ");
}

}  // namespace bigquery_emulator_duckdb::translator
