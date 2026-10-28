#include "src/translator/scan.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <iterator>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/match.h"
#include "absl/time/time.h"
#include "googlesql/public/catalog.h"
#include "googlesql/public/function.h"
#include "googlesql/public/table_valued_function.h"
#include "googlesql/public/type.h"
#include "googlesql/public/value.h"
#include "googlesql/resolved_ast/resolved_ast.h"
#include "src/catalog_table.h"
#include "src/connection_id.h"
#include "src/duckdb_sql.h"
#include "src/external_query.h"
#include "src/statement_resources.h"
#include "src/table_metadata.h"
#include "src/table_source.h"
#include "src/translator/aggregate.h"
#include "src/translator/collation.h"
#include "src/translator/context.h"
#include "src/translator/differential_privacy.h"
#include "src/translator/expression.h"
#include "src/translator/function.h"
#include "src/translator/graph.h"
#include "src/type_mapping.h"

namespace bigquery_emulator_duckdb::translator {
namespace {

constexpr auto kZoneIndependentFunctions = std::to_array<std::string_view>({
    "current_timestamp",
    "timestamp_add",
    "timestamp_sub",
});

// Converts an external result column to the BigQuery type the catalog gave it. MySQL decimals
// and PostgreSQL numerics beyond DuckDB's precision arrive as text. MySQL TIMESTAMP arrives as
// a UTC timestamp without a time zone. Dates and times outside BigQuery's ranges fail.
std::string ExternalValue(const googlesql::Type* type, const std::string& value) {
  std::string sql = "CAST(" + value + " AS " + DuckDbType(type).value_or("VARCHAR") + ")";
  const auto in_range = [&](const std::string& condition, std::string_view name) {
    return "CASE WHEN " + sql + " IS NULL OR " + condition + " THEN " + sql + " ELSE error('" +
           std::string(name) + " value out of range: ' || CAST(" + sql + " AS VARCHAR)) END";
  };
  switch (type->kind()) {
    case googlesql::TYPE_BIGNUMERIC:
      return "CAST(bq_bignumeric_from_string(CAST(" + value + " AS VARCHAR), false) AS BIGNUM)";
    case googlesql::TYPE_DATE:
      return in_range(sql + " BETWEEN DATE '0001-01-01' AND DATE '9999-12-31'", "DATE");
    case googlesql::TYPE_DATETIME:
      return in_range(sql +
                          " BETWEEN TIMESTAMP '0001-01-01 00:00:00' AND TIMESTAMP '9999-12-31 "
                          "23:59:59.999999'",
                      "DATETIME");
    case googlesql::TYPE_TIMESTAMP:
      return in_range(sql +
                          " BETWEEN TIMESTAMPTZ '0001-01-01 00:00:00+00' AND TIMESTAMPTZ "
                          "'9999-12-31 23:59:59.999999+00'",
                      "TIMESTAMP");
    case googlesql::TYPE_TIME:
      return in_range(sql + " < TIME '24:00:00'", "TIME");
    default:
      return sql;
  }
}

// EXTERNAL_QUERY reads from the database that the catalog attached when it analyzed the query.
std::optional<Relation> ExternalQueryScan(const googlesql::ResolvedTVFScan& tvf,
                                          const Scope& scope) {
  const auto& arguments = tvf.signature()->input_arguments();
  const auto literal = [&](size_t i) {
    return arguments.at(i).GetScalarArgType()->literal_value()->string_value();
  };
  // The catalog accepted the connection ID.
  const std::optional<std::string> key = ConnectionKey(literal(0), scope.context.defaults.project);
  if (!key) {
    return Unsupported(scope, "EXTERNAL_QUERY connection ID");
  }
  std::vector<std::string> names;
  names.reserve(static_cast<size_t>(tvf.signature()->result_schema().num_columns()));
  for (int i = 0; i < tvf.signature()->result_schema().num_columns(); ++i) {
    names.push_back(QuoteIdentifier("_x" + std::to_string(i)));
  }
  Relation result;
  std::vector<std::string> projections;
  for (int i = 0; i < tvf.column_list_size(); ++i) {
    const auto& column = tvf.column_list(i);
    const std::string alias = ColumnName(column.column_id());
    projections.push_back(ExternalValue(column.type(), "e." + names.at(static_cast<size_t>(
                                                                  tvf.column_index_list(i)))) +
                          " AS " + alias);
    result.columns.emplace(column.column_id(), "q." + alias);
  }
  result.sql = "SELECT " + (projections.empty() ? "1 AS _unit" : Join(projections, ", ")) +
               " FROM memory.main." + QuoteIdentifier(ExternalDatabaseName(*key)) + "(" +
               QuoteLiteral(literal(1)) + ") AS e(" + Join(names, ", ") + ")";
  return result;
}

std::optional<Relation> SessionizeScan(const googlesql::ResolvedTVFScan& tvf, const Scope& scope) {
  if (tvf.tvf()->Name() == "external_query") {
    return ExternalQueryScan(tvf, scope);
  }
  if (tvf.tvf()->Name() != "range_sessionize") {
    return Unsupported(scope, "table function " + tvf.tvf()->Name());
  }
  const auto& argument = *tvf.argument_list(0);
  const auto input = Scan(*argument.scan(), scope);
  if (!input) {
    return std::nullopt;
  }
  const auto& schema = tvf.signature()->input_arguments().at(0).relation();
  const auto range_name = tvf.signature()->input_arguments().at(1).GetScalarArgType();
  const auto partitions = tvf.signature()->input_arguments().at(2).GetScalarArgType();
  const auto partition_names = partitions->literal_value()->elements_view();
  std::vector<std::string> keys;
  std::string range;
  for (int i = 0; i < schema.num_columns(); ++i) {
    const auto& column = argument.argument_column_list(i);
    if (!DuckDbType(column.type()) || column.type_annotation_map() != nullptr) {
      return Unsupported(scope, "RANGE_SESSIONIZE column type");
    }
    const std::string name = ColumnName(column.column_id());
    // Names were validated by the catalog, including ambiguous input names.
    if (absl::EqualsIgnoreCase(schema.column(i).name,
                               range_name->literal_value()->string_value())) {
      range = "q." + name;
    }
    for (const auto& key : *partition_names) {
      if (absl::EqualsIgnoreCase(schema.column(i).name, key.string_value())) {
        if (HasInterval(column.type())) {
          return Unsupported(scope, "RANGE_SESSIONIZE partitioning column type");
        }
        keys.push_back(name);
      }
    }
  }
  const auto& arguments = tvf.signature()->input_arguments();
  const bool overlaps =
      arguments.size() == 4 &&
      arguments.at(3).GetScalarArgType()->literal_value()->EnumDisplayName() == "OVERLAPS";
  const std::string partition = keys.empty() ? "" : "PARTITION BY " + Join(keys, ", ") + " ";
  const std::string order =
      "ORDER BY _rs_start NULLS LAST, _rs_end NULLS LAST, _rs_id "
      "ROWS BETWEEN UNBOUNDED PRECEDING AND ";
  // RANGE's unbounded ends are stored as infinities. A running maximum (rather
  // than LAG) keeps nested ranges and chains in the same connected component.
  const std::string bounds = std::format(
      "SELECT q.*, row_number() OVER () AS _rs_id, {0}.{1} AS _rs_start, "
      "{0}.{2} AS _rs_end{3}",
      range, QuoteIdentifier(kRangeStart), QuoteIdentifier(kRangeEnd), input->From());
  const std::string previous =
      std::format("SELECT *, max(_rs_end) OVER ({}{}1 PRECEDING) AS _rs_previous FROM ({})",
                  partition, order, bounds);
  const std::string groups = std::format(
      "SELECT *, sum(CASE WHEN _rs_previous IS NULL OR _rs_start {} _rs_previous "
      "THEN 1 ELSE 0 END) OVER ({}{}CURRENT ROW) AS _rs_group FROM ({})",
      overlaps ? ">=" : ">", partition, order, previous);
  keys.emplace_back("_rs_group");
  const std::string window = " OVER (PARTITION BY " + Join(keys, ", ") + ")";
  const std::string session = std::format(
      "CASE WHEN {} IS NULL THEN NULL ELSE "
      "struct_pack({} := min(_rs_start){}, {} := max(_rs_end){}) END",
      range, QuoteIdentifier(kRangeStart), window, QuoteIdentifier(kRangeEnd), window);
  Relation result;
  std::vector<std::string> projections;
  for (int i = 0; i < tvf.column_list_size(); ++i) {
    const int index = tvf.column_index_list(i);
    const int id = tvf.column_list(i).column_id();
    const std::string value = absl::EqualsIgnoreCase(tvf.column_list(i).name(), "session_range")
                                  ? session
                                  : ColumnName(argument.argument_column_list(index).column_id());
    projections.push_back(value + " AS " + ColumnName(id));
    result.columns.emplace(id, "q." + ColumnName(id));
  }
  result.sql = "SELECT " + (projections.empty() ? "1 AS _unit" : Join(projections, ", ")) +
               " FROM (" + groups + ") AS q";
  return result;
}

// Projects `columns` of a relation, positionally renamed to `names`.
std::optional<std::vector<std::string>> Renamed(
    const Relation& relation, const std::vector<googlesql::ResolvedColumn>& columns,
    const std::vector<std::string>& names) {
  std::vector<std::string> projections;
  for (size_t i = 0; i < columns.size(); ++i) {
    const auto column = relation.columns.find(columns.at(i).column_id());
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
  // UNION compares rows by their columns' collations, which the resolved AST does not record.
  if (recursive.op_type() != googlesql::ResolvedRecursiveScan::UNION_ALL &&
      std::ranges::any_of(recursive.column_list(), [](const googlesql::ResolvedColumn& column) {
        return HasCollation(column.type_annotation_map());
      })) {
    return Unsupported(scope, "WITH RECURSIVE UNION DISTINCT with collation");
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
    const WithQuery query{.name = scope.context.FreshName("_w"), .width = names.size()};
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
    const int id = columns.at(i).column_id();
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
  using Operator =
      std::pair<googlesql::ResolvedSetOperationScan::SetOperationType, std::string_view>;
  static constexpr auto operators = std::to_array<Operator>({
      {googlesql::ResolvedSetOperationScan::UNION_ALL, " UNION ALL "},
      {googlesql::ResolvedSetOperationScan::UNION_DISTINCT, " UNION "},
      {googlesql::ResolvedSetOperationScan::INTERSECT_DISTINCT, " INTERSECT "},
      {googlesql::ResolvedSetOperationScan::EXCEPT_DISTINCT, " EXCEPT "},
  });
  // GoogleSQL accepts these forms, but BigQuery only supports DISTINCT for them.
  if (set.op_type() == googlesql::ResolvedSetOperationScan::INTERSECT_ALL ||
      set.op_type() == googlesql::ResolvedSetOperationScan::EXCEPT_ALL) {
    const std::string name = set.op_type() == googlesql::ResolvedSetOperationScan::INTERSECT_ALL
                                 ? "INTERSECT"
                                 : "EXCEPT";
    throw std::invalid_argument(name + " ALL is not supported. Only " + name +
                                " DISTINCT is supported.");
  }
  const auto* op = std::ranges::find(operators, set.op_type(), &Operator::first);
  if (op == operators.end() || set.column_list().empty()) {
    return std::nullopt;
  }
  const bool distinct = set.op_type() == googlesql::ResolvedSetOperationScan::UNION_DISTINCT ||
                        set.op_type() == googlesql::ResolvedSetOperationScan::INTERSECT_DISTINCT ||
                        set.op_type() == googlesql::ResolvedSetOperationScan::EXCEPT_DISTINCT;
  // DISTINCT compares rows by their columns' collations, which the resolved AST does not record.
  if (distinct &&
      std::ranges::any_of(set.column_list(), [](const googlesql::ResolvedColumn& column) {
        return HasCollation(column.type_annotation_map());
      })) {
    return Unsupported(scope, "set operation DISTINCT with collation");
  }
  Relation result;
  std::vector<std::string> names;
  bool intervals = false;
  for (const auto& column : set.column_list()) {
    if (distinct && column.type()->IsStruct()) {
      const std::string name =
          set.op_type() == googlesql::ResolvedSetOperationScan::UNION_DISTINCT       ? "UNION"
          : set.op_type() == googlesql::ResolvedSetOperationScan::INTERSECT_DISTINCT ? "INTERSECT"
                                                                                     : "EXCEPT";
      throw std::invalid_argument("Type STRUCT is not supported in " + name +
                                  " DISTINCT operation");
    }
    if (distinct && column.type()->IsArray()) {
      const std::string name = set.op_type() == googlesql::ResolvedSetOperationScan::UNION_DISTINCT
                                   ? "UNION DISTINCT"
                                   : "INTERSECT DISTINCT or EXCEPT DISTINCT";
      throw std::invalid_argument("Type ARRAY is not supported in " + name + " operation");
    }
    intervals = intervals || column.type()->IsInterval();
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
  if (set.op_type() == googlesql::ResolvedSetOperationScan::UNION_ALL || !intervals) {
    result.sql = Join(parts, op->second);
    return result;
  }
  // INTERVAL columns compare by their keys, and each distinct row is the first input row of its
  // keys, as BigQuery returns it.
  std::vector<std::string> compared;
  std::vector<std::string> keyed;
  std::vector<std::string> matches;
  for (int i = 0; i < set.column_list_size(); ++i) {
    const std::string& name = names.at(i);
    const bool interval = set.column_list(i).type()->IsInterval();
    compared.push_back(interval ? IntervalKey("r." + name) : "r." + name);
    keyed.push_back(
        std::format("{} AS {}", interval ? IntervalKey("q." + name) : "q." + name, name));
    matches.push_back(std::format("k.{} IS NOT DISTINCT FROM {}", name, compared.back()));
  }
  std::vector<std::string> keyed_parts;
  std::vector<std::string> rows;
  for (std::size_t i = 0; i < parts.size(); ++i) {
    keyed_parts.push_back(std::format("SELECT {} FROM ({}) AS q", Join(keyed, ", "), parts.at(i)));
    rows.push_back(std::format("SELECT q.*, {} AS _sb, row_number() OVER () AS _sr FROM ({}) AS q",
                               i, parts.at(i)));
  }
  std::string where = "r._sn = 1";
  if (set.op_type() != googlesql::ResolvedSetOperationScan::UNION_DISTINCT) {
    where += std::format(" AND EXISTS (SELECT 1 FROM ({}) AS k WHERE {})",
                         Join(keyed_parts, op->second), Join(matches, " AND "));
  }
  std::vector<std::string> outputs;
  std::ranges::transform(names, std::back_inserter(outputs),
                         [](const std::string& name) { return "r." + name; });
  result.sql = std::format(
      "SELECT {} FROM (SELECT r.*, row_number() OVER (PARTITION BY {} ORDER BY r._sb, r._sr) AS "
      "_sn FROM ({}) AS r) AS r WHERE {}",
      Join(outputs, ", "), Join(compared, ", "), Join(rows, " UNION ALL "), where);
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
                    ? std::optional<Relation>(
                          Relation{.sql = "SELECT 1 AS _unit", .columns = {}, .ordering = {}})
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

std::optional<Relation> SampleScan(const googlesql::ResolvedSampleScan& sample,
                                   const Scope& scope) {
  if (sample.method() != "system" || sample.unit() != googlesql::ResolvedSampleScan::PERCENT ||
      sample.repeatable_argument() != nullptr || sample.weight_column() != nullptr ||
      !sample.partition_by_list().empty()) {
    return Unsupported(scope, "TABLESAMPLE other than SYSTEM (PERCENT) without a suffix");
  }
  if (!sample.input_scan()->Is<googlesql::ResolvedTableScan>()) {
    return Unsupported(scope, "TABLESAMPLE on a subquery or array");
  }
  const auto& size = *sample.size();
  std::optional<googlesql::Value> value;
  if (size.Is<googlesql::ResolvedLiteral>()) {
    value = size.GetAs<googlesql::ResolvedLiteral>()->value();
  } else if (size.Is<googlesql::ResolvedParameter>() && scope.context.defaults.evaluate) {
    // DuckDB requires a numeric literal here, so evaluate the typed parameter first, as DDL
    // option values do, rather than extracting a number from its generated SQL.
    const auto sql = Expression(size, scope, {});
    if (!sql) {
      return std::nullopt;
    }
    value = scope.context.defaults.evaluate("SELECT " + *sql, size.type());
  } else {
    return Unsupported(scope, "TABLESAMPLE percentage");
  }
  if (value->is_null()) {
    throw std::invalid_argument("Const must be a DOUBLE-compatible constant.");
  }
  double percent =
      value->type()->IsInt64() ? static_cast<double>(value->int64_value()) : value->double_value();
  // BigQuery returned every row for a NaN parameter, including across multiple partitions.
  if (std::isnan(percent)) {
    percent = 100;
  }
  // The analyzer validates literals, but parameter values still need checking.
  if (percent < 0 || percent > 100) {
    throw std::invalid_argument("Sampling rate must be between 0 and 100 percent.");
  }
  auto result = Scan(*sample.input_scan(), scope);
  if (!result) {
    return std::nullopt;
  }
  // SYSTEM samples DuckDB vectors instead of BigQuery storage blocks; see README's exceptions.
  result->sql =
      "SELECT q.*" + result->From() + std::format(" TABLESAMPLE SYSTEM ({} PERCENT)", percent);
  return result;
}

// Whether `expression` has the same value in every time zone. BigQuery evaluates the time of FOR
// SYSTEM_TIME AS OF in America/Los_Angeles rather than in UTC, which the analyzer has already
// given a literal's time, so the emulator reads only times that do not depend on the zone.
bool ZoneIndependent(const googlesql::ResolvedExpr& expression) {
  if (expression.Is<googlesql::ResolvedParameter>() ||
      expression.Is<googlesql::ResolvedConstant>()) {
    return true;
  }
  if (expression.Is<googlesql::ResolvedLiteral>()) {
    return !expression.type()->IsTimestamp() ||
           expression.GetAs<googlesql::ResolvedLiteral>()->value().is_null();
  }
  if (!expression.Is<googlesql::ResolvedFunctionCall>()) {
    return false;
  }
  const auto& call = *expression.GetAs<googlesql::ResolvedFunctionCall>();
  const std::string name = call.function()->Name();
  return std::ranges::find(kZoneIndependentFunctions, name) != kZoneIndependentFunctions.end() &&
         std::ranges::all_of(call.argument_list(),
                             [](const auto& argument) { return ZoneIndependent(*argument); });
}

}  // namespace

std::optional<std::string> TimeTravelRows(const googlesql::ResolvedTableScan& scan,
                                          const Scope& scope, int64_t& time) {
  const auto* stored = dynamic_cast<const BigQueryTable*>(scan.table());
  TableSource* source = scope.context.defaults.source;
  const auto& expression = *scan.for_system_time_expr();
  if (stored == nullptr || source == nullptr || !scope.context.defaults.evaluate ||
      !expression.type()->IsTimestamp()) {
    return Unsupported(scope, "FOR SYSTEM_TIME AS OF this table");
  }
  if (stored->WriteRestriction() == TableWriteRestriction::kExternalNumericStorage) {
    return Unsupported(scope, "FOR SYSTEM_TIME AS OF external numeric storage");
  }
  if (!ZoneIndependent(expression)) {
    return Unsupported(scope,
                       "FOR SYSTEM_TIME AS OF a time other than a parameter, a variable or "
                       "CURRENT_TIMESTAMP with TIMESTAMP_ADD or TIMESTAMP_SUB");
  }
  // A query of the view reads the table at the time it evaluates, not at the view's creation.
  if (scope.context.defines_view) {
    return "SELECT * FROM " + QuoteIdentifierPath(stored->FullName()) +
           " WHERE error('The emulator reads FOR SYSTEM_TIME AS OF in a view only through its "
           "GoogleSQL query')";
  }
  // The time is a constant, which the emulator evaluates to find the rows the table had then.
  const auto sql = Expression(expression, scope, {});
  if (!sql) {
    return std::nullopt;
  }
  const googlesql::Value value =
      scope.context.defaults.evaluate("SELECT " + *sql, expression.type());
  if (value.is_null()) {
    throw std::invalid_argument("Query error: 'FOR SYSTEM_TIME AS OF' expression for table '" +
                                stored->reference().project_id + "." +
                                stored->reference().dataset_id + "." +
                                stored->reference().table_id + "' is NULL.");
  }
  time = absl::ToUnixMicros(value.ToUnixPicos().ToAbslTime());
  absl::StatusOr<std::string> rows = source->TimeTravelQuery(stored->reference(), time);
  if (absl::IsUnimplemented(rows.status())) {
    return Unsupported(scope, rows.status().message());
  }
  if (!rows.ok()) {
    throw std::invalid_argument(std::string(rows.status().message()));
  }
  return *std::move(rows);
}

namespace {

std::optional<Relation> ScanBody(const googlesql::ResolvedScan& scan, const Scope& scope) {
  if (scan.Is<googlesql::ResolvedGraphTableScan>()) {
    return GraphTableScan(*scan.GetAs<googlesql::ResolvedGraphTableScan>(), scope);
  }
  if (scan.Is<googlesql::ResolvedGraphScan>()) {
    return GraphScan(*scan.GetAs<googlesql::ResolvedGraphScan>(), scope);
  }
  if (scan.Is<googlesql::ResolvedGraphRefScan>()) {
    if (!scope.graph_input) {
      return Unsupported(scope, "graph input reference");
    }
    return *scope.graph_input;
  }
  if (scan.Is<googlesql::ResolvedGraphLinearScan>()) {
    Scope linear = scope;
    for (const auto& step : scan.GetAs<googlesql::ResolvedGraphLinearScan>()->scan_list()) {
      auto result = Scan(*step, linear);
      if (!result) {
        return std::nullopt;
      }
      linear.graph_input = std::make_shared<Relation>(*std::move(result));
    }
    return linear.graph_input ? std::optional<Relation>(*linear.graph_input) : std::nullopt;
  }
  if (scan.Is<googlesql::ResolvedGraphCallScan>()) {
    return Unsupported(scope, "the GQL CALL statement");
  }
  if (scan.Is<googlesql::ResolvedSingleRowScan>()) {
    return Relation{.sql = "SELECT 1 AS _unit", .columns = {}, .ordering = {}};
  }
  if (scan.Is<googlesql::ResolvedTableScan>()) {
    const auto* table = scan.GetAs<googlesql::ResolvedTableScan>();
    if (table->lock_mode() != nullptr || table->read_as_row_type() ||
        table->table()->IsValueTable() ||
        table->column_list_size() != table->column_index_list_size()) {
      return Unsupported(scope, "table read with a lock mode or a value table");
    }
    std::optional<std::string> history;
    if (table->for_system_time_expr() != nullptr) {
      int64_t time = 0;
      history = TimeTravelRows(*table, scope, time);
      if (!history) {
        return std::nullopt;
      }
    }
    Relation result;
    std::vector<std::string> projections;
    for (int i = 0; i < table->column_list_size(); ++i) {
      const auto& column = table->column_list(i);
      if (!DuckDbType(column.type()) || !OnlyCollations(column.type_annotation_map())) {
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
    const auto* stored_table = dynamic_cast<const BigQueryTable*>(table->table());
    const auto read_query =
        history || stored_table == nullptr
            ? std::nullopt
            : StatementResources::ReadQuery(stored_table->reference(), stored_table->ReadQuery(),
                                            scope.context.resources);
    result.sql = "SELECT " + (projections.empty() ? "1 AS _unit" : Join(projections, ", ")) +
                 " FROM " +
                 (history                ? "(" + *history + ") AS v"
                  : sql_table != nullptr ? "(" + sql_table->sql() + ") AS v"
                  : read_query           ? "(" + *read_query + ") AS v"
                                         : QuoteIdentifierPath(table->table()->FullName()));
    return result;
  }
  if (scan.Is<googlesql::ResolvedTVFScan>()) {
    return SessionizeScan(*scan.GetAs<googlesql::ResolvedTVFScan>(), scope);
  }
  if (scan.Is<googlesql::ResolvedJoinScan>()) {
    return JoinScan(*scan.GetAs<googlesql::ResolvedJoinScan>(), scope);
  }
  if (scan.Is<googlesql::ResolvedAggregateScan>()) {
    return AggregateScan(*scan.GetAs<googlesql::ResolvedAggregateScan>(), scope);
  }
  if (scan.Is<googlesql::ResolvedDifferentialPrivacyAggregateScan>()) {
    return DifferentialPrivacyAggregateScan(
        *scan.GetAs<googlesql::ResolvedDifferentialPrivacyAggregateScan>(), scope);
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
  if (scan.Is<googlesql::ResolvedSampleScan>()) {
    return SampleScan(*scan.GetAs<googlesql::ResolvedSampleScan>(), scope);
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
    const auto checked = [](const std::string& sql) {
      return "bq_checked_integer(" + sql +
             ", 0, 'Limit requires non-null count and offset', "
             "'Limit requires non-negative count and offset')";
    };
    result->sql = "SELECT q.*" + from + result->Order() + " LIMIT " + checked(*count);
    if (limit->offset() != nullptr) {
      const auto offset = Expression(*limit->offset(), scope, {});
      if (!offset) {
        return std::nullopt;
      }
      result->sql += " OFFSET " + checked(*offset);
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
  // Ordering from a subquery is not a promise of ordering for its parent scan. A GQL query's
  // ORDER BY marks only its OrderByScan. The next statement's reference to it, such as LIMIT's,
  // and the linear and graph table scans that output it keep its order, as the reference
  // implementation's do.
  if (result && !scan.is_ordered() && !scan.Is<googlesql::ResolvedGraphRefScan>() &&
      !scan.Is<googlesql::ResolvedGraphLinearScan>() &&
      !scan.Is<googlesql::ResolvedGraphTableScan>()) {
    result->ordering.clear();
  }
  return result;
}

std::string StoredColumn(const std::string& column, const googlesql::Type* type) {
  if (!HasBigNumeric(type) && !HasGeography(type)) {
    return column;
  }
  return "CAST(" + column + " AS " + DuckDbType(type).value_or("") + ")";
}

}  // namespace bigquery_emulator_duckdb::translator
