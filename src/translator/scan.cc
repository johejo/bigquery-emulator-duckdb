#include "src/translator/scan.h"

#include <cstddef>
#include <format>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "googlesql/public/catalog.h"
#include "googlesql/public/type.h"
#include "googlesql/resolved_ast/resolved_ast.h"
#include "src/catalog.h"
#include "src/duckdb_sql.h"
#include "src/translator/aggregate.h"
#include "src/translator/context.h"
#include "src/translator/expression.h"
#include "src/translator/function.h"
#include "src/type_mapping.h"

namespace bigquery_emulator_duckdb::translator {
namespace {

// Projects `columns` of a relation, positionally renamed to `names`.
std::optional<std::vector<std::string>> Renamed(
    const Relation& relation, const std::vector<googlesql::ResolvedColumn>& columns,
    const std::vector<std::string>& names) {
  std::vector<std::string> projections;
  for (size_t i = 0; i < columns.size(); ++i) {
    const auto column = relation.columns.find(columns[i].column_id());
    if (column == relation.columns.end()) {
      return std::nullopt;
    }
    projections.push_back(column->second + " AS " + names.at(i));
  }
  return projections;
}

std::optional<Relation> JoinScan(const googlesql::ResolvedJoinScan& join, const Scope& scope) {
  const auto left = Scan(*join.left_scan(), scope);
  if (!left) {
    return std::nullopt;
  }
  // A lateral right side sees the left's columns the way a correlated subquery sees its
  // enclosing query's.
  const auto right =
      Scan(*join.right_scan(), join.is_lateral() ? Nested(scope, left->columns) : scope);
  if (!right) {
    return std::nullopt;
  }
  Relation result;
  Columns visible;
  std::vector<std::string> projections;
  for (const auto& [side, relation] : {std::pair{"l.", &*left}, std::pair{"r.", &*right}}) {
    for (const auto& [id, sql] : relation->columns) {
      visible.emplace(id, side + ColumnName(id));
      projections.push_back(side + ColumnName(id) + " AS " + ColumnName(id));
      result.columns.emplace(id, "q." + ColumnName(id));
    }
  }
  std::string kind;
  switch (join.join_type()) {
    case googlesql::ResolvedJoinScan::INNER:
      kind = "INNER";
      break;
    case googlesql::ResolvedJoinScan::LEFT:
      kind = "LEFT";
      break;
    case googlesql::ResolvedJoinScan::RIGHT:
      kind = "RIGHT";
      break;
    case googlesql::ResolvedJoinScan::FULL:
      kind = "FULL";
      break;
    default:
      return Unsupported(scope, "join type");
  }
  std::string condition = " ON TRUE";
  if (join.join_expr() != nullptr) {
    const auto on = Expression(*join.join_expr(), scope, visible);
    if (!on) {
      return std::nullopt;
    }
    condition = " ON " + *on;
  } else if (kind == "INNER") {
    kind = "CROSS";
    condition.clear();
  }
  result.sql = "SELECT " + (projections.empty() ? "1 AS _unit" : Join(projections, ", ")) +
               " FROM (" + left->sql + ") AS l " + kind + " JOIN " +
               (join.is_lateral() ? "LATERAL " : "") + "(" + right->sql + ") AS r" + condition;
  return result;
}

// The body of a recursive WITH entry: DuckDB evaluates UNION [ALL] in WITH RECURSIVE by the
// same iteration, deduplicating against every earlier row for UNION.
std::optional<std::string> RecursiveQuery(const googlesql::ResolvedRecursiveScan& recursive,
                                          const std::vector<std::string>& names,
                                          const WithQuery& self, const Scope& scope) {
  if (recursive.recursion_depth_modifier() != nullptr) {
    return Unsupported(scope, "WITH RECURSIVE depth modifier");
  }
  std::vector<std::string> terms;
  for (const auto* item : {recursive.non_recursive_term(), recursive.recursive_term()}) {
    Scope inner = scope;
    if (item == recursive.recursive_term()) {
      inner.recursive = self;
    }
    const auto relation = Scan(*item->scan(), inner);
    if (!relation) {
      return std::nullopt;
    }
    const auto projections = Renamed(*relation, item->output_column_list(), names);
    if (!projections || projections->empty()) {
      return std::nullopt;
    }
    terms.push_back("SELECT " + Join(*projections, ", ") + relation->From());
  }
  return Join(terms, recursive.op_type() == googlesql::ResolvedRecursiveScan::UNION_ALL
                         ? " UNION ALL "
                         : " UNION ");
}

std::optional<Relation> WithScan(const googlesql::ResolvedWithScan& with, const Scope& scope) {
  Scope inner = scope;
  std::vector<std::string> definitions;
  for (const auto& entry : with.with_entry_list()) {
    const auto* subquery = entry->with_subquery();
    std::vector<std::string> names;
    names.reserve(subquery->column_list_size());
    for (int i = 0; i < subquery->column_list_size(); ++i) {
      names.push_back(QuoteIdentifier("_p" + std::to_string(i)));
    }
    const WithQuery query{scope.context.FreshName("_w"), names.size()};
    std::optional<std::string> body;
    if (subquery->Is<googlesql::ResolvedRecursiveScan>()) {
      body =
          RecursiveQuery(*subquery->GetAs<googlesql::ResolvedRecursiveScan>(), names, query, inner);
    } else {
      const auto relation = Scan(*subquery, inner);
      if (!relation) {
        return std::nullopt;
      }
      const auto projections = Renamed(*relation, subquery->column_list(), names);
      if (!projections || projections->empty()) {
        return std::nullopt;
      }
      body = "SELECT " + Join(*projections, ", ") + relation->From();
    }
    if (!body) {
      return std::nullopt;
    }
    definitions.push_back(
        std::format("{}({}) AS ({})", QuoteIdentifier(query.name), Join(names, ", "), *body));
    inner.with[entry->with_query_name()] = query;
  }
  auto result = Scan(*with.query(), inner);
  if (!result) {
    return std::nullopt;
  }
  result->sql = std::string(with.recursive() ? "WITH RECURSIVE " : "WITH ") +
                Join(definitions, ", ") + " SELECT q.*" + result->From();
  return result;
}

// Reads `query` into fresh columns.
Relation WithRead(const WithQuery& query, const std::vector<googlesql::ResolvedColumn>& columns) {
  Relation result;
  std::vector<std::string> projections;
  for (size_t i = 0; i < columns.size(); ++i) {
    const int id = columns[i].column_id();
    projections.push_back("q." + QuoteIdentifier("_p" + std::to_string(i)) + " AS " +
                          ColumnName(id));
    result.columns.emplace(id, "q." + ColumnName(id));
  }
  result.sql =
      std::format("SELECT {} FROM {} AS q", Join(projections, ", "), QuoteIdentifier(query.name));
  return result;
}

std::optional<Relation> WithRefScan(const googlesql::ResolvedWithRefScan& ref, const Scope& scope) {
  const auto with = scope.with.find(ref.with_query_name());
  if (with == scope.with.end() || std::cmp_not_equal(with->second.width, ref.column_list_size())) {
    return std::nullopt;
  }
  return WithRead(with->second, ref.column_list());
}

std::optional<Relation> RecursiveRefScan(const googlesql::ResolvedRecursiveRefScan& ref,
                                         const Scope& scope) {
  if (!scope.recursive || std::cmp_not_equal(scope.recursive->width, ref.column_list_size())) {
    return std::nullopt;
  }
  return WithRead(*scope.recursive, ref.column_list());
}

std::optional<Relation> SetOperationScan(const googlesql::ResolvedSetOperationScan& set,
                                         const Scope& scope) {
  // CORRESPONDING is resolved into each input item's output_column_list, which is positional
  // and pads missing columns with NULLs, so the match and propagation modes need no handling.
  static const std::map<googlesql::ResolvedSetOperationScan::SetOperationType, std::string>
      operators = {{googlesql::ResolvedSetOperationScan::UNION_ALL, " UNION ALL "},
                   {googlesql::ResolvedSetOperationScan::UNION_DISTINCT, " UNION "},
                   {googlesql::ResolvedSetOperationScan::INTERSECT_ALL, " INTERSECT ALL "},
                   {googlesql::ResolvedSetOperationScan::INTERSECT_DISTINCT, " INTERSECT "},
                   {googlesql::ResolvedSetOperationScan::EXCEPT_ALL, " EXCEPT ALL "},
                   {googlesql::ResolvedSetOperationScan::EXCEPT_DISTINCT, " EXCEPT "}};
  const auto op = operators.find(set.op_type());
  if (op == operators.end() || set.column_list().empty()) {
    return std::nullopt;
  }
  Relation result;
  std::vector<std::string> names;
  for (const auto& column : set.column_list()) {
    if (set.op_type() != googlesql::ResolvedSetOperationScan::UNION_ALL &&
        HasInterval(column.type())) {
      return Unsupported(scope, "set operation with INTERVAL");
    }
    names.push_back(ColumnName(column.column_id()));
    result.columns.emplace(column.column_id(), "q." + names.back());
  }
  std::vector<std::string> parts;
  for (const auto& item : set.input_item_list()) {
    const auto relation = Scan(*item->scan(), scope);
    if (!relation || item->output_column_list().size() != names.size()) {
      return std::nullopt;
    }
    const auto projections = Renamed(*relation, item->output_column_list(), names);
    if (!projections) {
      return std::nullopt;
    }
    parts.push_back("SELECT " + Join(*projections, ", ") + relation->From());
  }
  result.sql = Join(parts, op->second);
  return result;
}

// The joined tail of an array scan: the lateral UNNEST `unnest` with its element and offset
// columns `visible` to the join condition.
std::optional<Relation> JoinArrays(const googlesql::ResolvedArrayScan& array, Relation input,
                                   const std::string& unnest, Columns visible,
                                   const std::vector<std::string>& elements, const Scope& scope) {
  std::vector<std::string> projections = {"q.*"};
  for (int i = 0; i < array.element_column_list_size(); ++i) {
    const int element = array.element_column_list(i).column_id();
    projections.push_back(elements.at(i) + " AS " + ColumnName(element));
    visible.emplace(element, elements.at(i));
    input.columns.emplace(element, "q." + ColumnName(element));
  }
  if (array.array_offset_column() != nullptr) {
    const int offset = array.array_offset_column()->column().column_id();
    projections.push_back("CAST(u.o - 1 AS BIGINT) AS " + ColumnName(offset));
    visible.emplace(offset, "(u.o - 1)");
    input.columns.emplace(offset, "q." + ColumnName(offset));
  }
  std::string condition = "TRUE";
  if (array.join_expr() != nullptr) {
    const auto on = Expression(*array.join_expr(), scope, visible);
    if (!on) {
      return std::nullopt;
    }
    condition = *on;
  }
  input.sql = "SELECT " + Join(projections, ", ") + input.From() +
              (array.is_outer() ? " LEFT" : " INNER") + " JOIN LATERAL " + unnest + " ON " +
              condition;
  return input;
}

// UNNEST(a, b, mode => ...) walks the arrays in step by position. The row count is the longest
// array's for PAD, which pads the others with NULL, and the shortest's for TRUNCATE; STRICT
// fails when the lengths differ. A NULL array counts as empty.
std::optional<Relation> ZippedArrayScan(const googlesql::ResolvedArrayScan& array,
                                        const Relation& input, const Scope& scope) {
  std::string mode = "PAD";
  if (const auto* zip = array.array_zip_mode(); zip != nullptr) {
    if (!zip->Is<googlesql::ResolvedLiteral>() ||
        zip->GetAs<googlesql::ResolvedLiteral>()->value().is_null() || !zip->type()->IsEnum()) {
      return Unsupported(scope, "UNNEST mode that is not a literal");
    }
    mode = zip->GetAs<googlesql::ResolvedLiteral>()->value().EnumDisplayName();
  }
  std::vector<std::string> arrays;
  std::vector<std::string> lengths;
  std::vector<std::string> elements;
  std::vector<std::string> picks;
  for (int i = 0; i < array.array_expr_list_size(); ++i) {
    const auto sql = Expression(*array.array_expr_list(i), scope, input.columns);
    if (!sql) {
      return std::nullopt;
    }
    const std::string name = "a" + std::to_string(i);
    arrays.push_back(*sql + " AS " + name);
    lengths.push_back("coalesce(len(z." + name + "), 0)");
    elements.push_back("u.e" + std::to_string(i));
    picks.push_back("z." + name + "[u.o] AS e" + std::to_string(i));
  }
  std::string count;
  if (mode == "PAD") {
    count = "greatest(" + Join(lengths, ", ") + ")";
  } else if (mode == "TRUNCATE") {
    count = "least(" + Join(lengths, ", ") + ")";
  } else if (mode == "STRICT") {
    count = std::format(
        "CASE WHEN least({0}) = greatest({0}) THEN greatest({0})"
        " ELSE error('Unnested arrays under STRICT mode must have equal lengths') END",
        Join(lengths, ", "));
  } else {
    return Unsupported(scope, "UNNEST mode " + mode);
  }
  picks.emplace_back("u.o");
  const std::string unnest =
      std::format("(SELECT {} FROM (SELECT {}) AS z, range(1, {} + 1) AS u(o)) AS u",
                  Join(picks, ", "), Join(arrays, ", "), count);
  return JoinArrays(array, input, unnest, input.columns, elements, scope);
}

std::optional<Relation> ArrayScan(const googlesql::ResolvedArrayScan& array, const Scope& scope) {
  auto result = array.input_scan() == nullptr
                    ? std::optional<Relation>(Relation{"SELECT 1 AS _unit", {}, {}})
                    : Scan(*array.input_scan(), scope);
  if (!result) {
    return std::nullopt;
  }
  if (array.array_expr_list_size() != 1) {
    return ZippedArrayScan(array, *result, scope);
  }
  const auto elements = Expression(*array.array_expr_list(0), scope, result->columns);
  if (!elements) {
    return std::nullopt;
  }
  // UNNEST is joined laterally so the array can refer to the input row. Ordinals are 1-based.
  return JoinArrays(array, *result, "unnest(" + *elements + ") WITH ORDINALITY AS u(e, o)",
                    result->columns, {"u.e"}, scope);
}

std::optional<Relation> ScanBody(const googlesql::ResolvedScan& scan, const Scope& scope) {
  if (scan.Is<googlesql::ResolvedSingleRowScan>()) {
    return Relation{"SELECT 1 AS _unit", {}, {}};
  }
  if (scan.Is<googlesql::ResolvedTableScan>()) {
    const auto* table = scan.GetAs<googlesql::ResolvedTableScan>();
    if (table->for_system_time_expr() != nullptr || table->lock_mode() != nullptr ||
        table->read_as_row_type() || table->table()->IsValueTable() ||
        table->column_list_size() != table->column_index_list_size()) {
      return Unsupported(scope, "table read with FOR SYSTEM_TIME, a lock mode or a value table");
    }
    Relation result;
    std::vector<std::string> projections;
    for (int i = 0; i < table->column_list_size(); ++i) {
      const auto& column = table->column_list(i);
      if (!DuckDbType(column.type()) || column.type_annotation_map() != nullptr) {
        return Unsupported(scope, "column type " + column.type()->DebugString());
      }
      const std::string alias = ColumnName(column.column_id());
      projections.push_back(
          StoredColumn(
              QuoteIdentifier(table->table()->GetColumn(table->column_index_list(i))->Name()),
              column.type()) +
          " AS " + alias);
      result.columns.emplace(column.column_id(), "q." + alias);
    }
    // A view such as INFORMATION_SCHEMA.TABLES is read from the query that computes it.
    const auto* sql_table = dynamic_cast<const SqlTable*>(table->table());
    result.sql = "SELECT " + (projections.empty() ? "1 AS _unit" : Join(projections, ", ")) +
                 " FROM " +
                 (sql_table != nullptr ? "(" + sql_table->sql() + ") AS v"
                                       : QuoteIdentifierPath(table->table()->FullName()));
    return result;
  }
  if (scan.Is<googlesql::ResolvedJoinScan>()) {
    return JoinScan(*scan.GetAs<googlesql::ResolvedJoinScan>(), scope);
  }
  if (scan.Is<googlesql::ResolvedAggregateScan>()) {
    return AggregateScan(*scan.GetAs<googlesql::ResolvedAggregateScan>(), scope);
  }
  if (scan.Is<googlesql::ResolvedAnalyticScan>()) {
    return AnalyticScan(*scan.GetAs<googlesql::ResolvedAnalyticScan>(), scope);
  }
  if (scan.Is<googlesql::ResolvedWithScan>()) {
    return WithScan(*scan.GetAs<googlesql::ResolvedWithScan>(), scope);
  }
  if (scan.Is<googlesql::ResolvedRecursiveRefScan>()) {
    return RecursiveRefScan(*scan.GetAs<googlesql::ResolvedRecursiveRefScan>(), scope);
  }
  if (scan.Is<googlesql::ResolvedWithRefScan>()) {
    return WithRefScan(*scan.GetAs<googlesql::ResolvedWithRefScan>(), scope);
  }
  if (scan.Is<googlesql::ResolvedSetOperationScan>()) {
    return SetOperationScan(*scan.GetAs<googlesql::ResolvedSetOperationScan>(), scope);
  }
  if (scan.Is<googlesql::ResolvedArrayScan>()) {
    return ArrayScan(*scan.GetAs<googlesql::ResolvedArrayScan>(), scope);
  }
  const googlesql::ResolvedScan* input = nullptr;
  if (scan.Is<googlesql::ResolvedProjectScan>()) {
    input = scan.GetAs<googlesql::ResolvedProjectScan>()->input_scan();
  }
  if (scan.Is<googlesql::ResolvedFilterScan>()) {
    input = scan.GetAs<googlesql::ResolvedFilterScan>()->input_scan();
  }
  if (scan.Is<googlesql::ResolvedOrderByScan>()) {
    input = scan.GetAs<googlesql::ResolvedOrderByScan>()->input_scan();
  }
  if (scan.Is<googlesql::ResolvedLimitOffsetScan>()) {
    input = scan.GetAs<googlesql::ResolvedLimitOffsetScan>()->input_scan();
  }
  if (input == nullptr) {
    return Unsupported(scope, "scan " + scan.node_kind_string());
  }
  auto result = Scan(*input, scope);
  if (!result) {
    return std::nullopt;
  }
  const std::string from = result->From();
  if (scan.Is<googlesql::ResolvedProjectScan>()) {
    const auto* project = scan.GetAs<googlesql::ResolvedProjectScan>();
    const Columns input_columns = result->columns;
    std::vector<std::string> projections;
    for (const auto& [id, sql] : input_columns) {
      projections.push_back(sql + " AS " + ColumnName(id));
    }
    for (const auto& computed : project->expr_list()) {
      const auto expression = Expression(*computed->expr(), scope, input_columns);
      if (!expression) {
        return std::nullopt;
      }
      const int id = computed->column().column_id();
      projections.push_back(*expression + " AS " + ColumnName(id));
      result->columns.emplace(id, "q." + ColumnName(id));
    }
    result->sql = "SELECT " + (projections.empty() ? "1 AS _unit" : Join(projections, ", ")) + from;
  } else if (scan.Is<googlesql::ResolvedFilterScan>()) {
    const auto filter = Expression(*scan.GetAs<googlesql::ResolvedFilterScan>()->filter_expr(),
                                   scope, result->columns);
    if (!filter) {
      return std::nullopt;
    }
    result->sql = "SELECT q.*" + from + " WHERE " + *filter;
  } else if (scan.Is<googlesql::ResolvedOrderByScan>()) {
    const auto order = OrderItems(
        scan.GetAs<googlesql::ResolvedOrderByScan>()->order_by_item_list(), scope, result->columns);
    if (!order) {
      return std::nullopt;
    }
    result->ordering = {*order};
  } else {
    const auto* limit = scan.GetAs<googlesql::ResolvedLimitOffsetScan>();
    const auto count = Expression(*limit->limit(), scope, {});
    if (!count) {
      return std::nullopt;
    }
    result->sql = "SELECT q.*" + from + result->Order() + " LIMIT " + *count;
    if (limit->offset() != nullptr) {
      const auto offset = Expression(*limit->offset(), scope, {});
      if (!offset) {
        return std::nullopt;
      }
      result->sql += " OFFSET " + *offset;
    }
  }
  return result;
}

}  // namespace

std::optional<Relation> Scan(const googlesql::ResolvedScan& scan, const Scope& scope) {
  if (!scan.hint_list().empty()) {
    return Unsupported(scope, "hints");
  }
  auto result = ScanBody(scan, scope);
  // Ordering from a subquery is not a promise of ordering for its parent scan.
  if (result && !scan.is_ordered()) {
    result->ordering.clear();
  }
  return result;
}

std::string StoredColumn(const std::string& column, const googlesql::Type* type) {
  if (!HasBigNumeric(type)) {
    return column;
  }
  return "CAST(" + column + " AS " + DuckDbType(type).value_or("") + ")";
}

}  // namespace bigquery_emulator_duckdb::translator
