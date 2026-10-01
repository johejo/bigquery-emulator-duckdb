#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "googlesql/public/function.h"
#include "googlesql/public/type.h"
#include "googlesql/resolved_ast/resolved_ast.h"
#include "src/duckdb_sql.h"
#include "src/translator/internal.h"

namespace bigquery_emulator_duckdb::translator {
namespace {

// Aggregate calls (with `aggregate` set) and analytic calls (with a non-empty `over`). An
// aggregate's HAVING MAX/MIN modifier is the caller's to compute, as the rows `having` selects.
std::optional<std::string> NonScalarCall(const googlesql::ResolvedNonScalarFunctionCallBase& call,
                                         const googlesql::ResolvedAggregateFunctionCall* aggregate,
                                         const std::string& over, const Scope& scope,
                                         const Columns& columns, const std::string& having = "") {
  const std::string name = ToUpperAscii(call.function()->Name());
  if (!call.generic_argument_list().empty() || !call.hint_list().empty() ||
      !call.collation_list().empty() ||
      call.error_mode() != googlesql::ResolvedFunctionCallBase::DEFAULT_ERROR_MODE ||
      (!call.function()->IsGoogleSQLBuiltin() && name != "MAX_BY" && name != "MIN_BY") ||
      call.where_expr() != nullptr) {
    return Unsupported(scope, "aggregate or analytic function " + name + " with modifiers");
  }
  if (aggregate != nullptr &&
      ((aggregate->having_modifier() != nullptr && having.empty()) ||
       !aggregate->group_by_list().empty() || !aggregate->group_by_aggregate_list().empty() ||
       aggregate->having_expr() != nullptr)) {
    return Unsupported(scope, "aggregate " + name + " with HAVING or GROUP BY");
  }
  static const std::map<std::string, std::string> aggregates = {
      {"COUNT", "count"},
      {"$COUNT_STAR", "count"},
      {"SUM", "sum"},
      {"AVG", "avg"},
      {"MIN", "min"},
      {"MAX", "max"},
      {"ANY_VALUE", "any_value"},
      {"ARRAY_AGG", "list"},
      {"ARRAY_CONCAT_AGG", "list"},
      // Exact, which is within any approximation error.
      {"APPROX_COUNT_DISTINCT", "count"},
      {"APPROX_QUANTILES", "quantile_disc"},
      {"APPROX_TOP_COUNT", "histogram"},
      // The _null variants return a NULL x instead of skipping its row.
      {"MAX_BY", "arg_max_null"},
      {"MIN_BY", "arg_min_null"},
      {"STRING_AGG", "string_agg"},
      {"COUNTIF", "count_if"},
      {"LOGICAL_AND", "bool_and"},
      {"LOGICAL_OR", "bool_or"},
      {"BIT_AND", "bit_and"},
      {"BIT_OR", "bit_or"},
      {"BIT_XOR", "bit_xor"},
      {"STDDEV", "stddev_samp"},
      {"STDDEV_SAMP", "stddev_samp"},
      {"STDDEV_POP", "stddev_pop"},
      {"VARIANCE", "var_samp"},
      {"VAR_SAMP", "var_samp"},
      {"VAR_POP", "var_pop"},
      {"CORR", "corr"},
      {"COVAR_POP", "covar_pop"},
      {"COVAR_SAMP", "covar_samp"}};
  static const std::map<std::string, std::string> analytics = {{"ROW_NUMBER", "row_number"},
                                                               {"RANK", "rank"},
                                                               {"DENSE_RANK", "dense_rank"},
                                                               {"PERCENT_RANK", "percent_rank"},
                                                               {"CUME_DIST", "cume_dist"},
                                                               {"NTILE", "ntile"},
                                                               {"LAG", "lag"},
                                                               {"LEAD", "lead"},
                                                               {"FIRST_VALUE", "first_value"},
                                                               {"LAST_VALUE", "last_value"},
                                                               {"NTH_VALUE", "nth_value"}};
  auto function = aggregates.find(name);
  if (function == aggregates.end()) {
    function = analytics.find(name);
    if (function == analytics.end() || over.empty()) {
      return Unsupported(scope, "aggregate or analytic function " + name);
    }
  }
  std::vector<std::string> args;
  for (const auto& argument : call.argument_list()) {
    const auto sql = Expression(*argument, scope, columns);
    if (!sql) {
      return std::nullopt;
    }
    args.push_back(*sql);
  }
  // DuckDB's string_agg() only joins strings, so STRING_AGG over BYTES joins their hexadecimal
  // digits; the default delimiter is b','.
  const bool bytes = name == "STRING_AGG" && call.argument_list(0)->type()->IsBytes();
  if (bytes) {
    args.at(0) = "hex(" + args.at(0) + ")";
    if (args.size() > 1) {
      args.at(1) = "hex(" + args.at(1) + ")";
    } else {
      args.emplace_back("'2C'");
    }
  }
  const auto unhex = [bytes](const std::string& sql) { return bytes ? "unhex(" + sql + ")" : sql; };
  // APPROX_QUANTILES(x, n) takes the n + 1 quantiles 0, 1/n, ..., 1.
  if (name == "APPROX_QUANTILES") {
    args.at(1) = "list_transform(range(" + args.at(1) + " + 1), lambda i: i / " + args.at(1) + ")";
  }
  // APPROX_TOP_COUNT's histogram() takes only the values; the count is applied afterwards.
  const std::string top = name == "APPROX_TOP_COUNT" ? args.at(1) : "";
  if (!top.empty()) {
    if (call.distinct()) {
      return Unsupported(scope, "APPROX_TOP_COUNT(DISTINCT ...)");
    }
    args.pop_back();
  }
  std::string inner = name == "$COUNT_STAR" ? "*" : Join(args, ", ");
  if (call.distinct() || name == "APPROX_COUNT_DISTINCT") {
    inner = "DISTINCT " + inner;
  }
  std::vector<std::string> conditions;
  if (!having.empty()) {
    conditions.push_back(having);
  }
  if (call.null_handling_modifier() == googlesql::ResolvedNonScalarFunctionCallBase::IGNORE_NULLS) {
    if (name == "ARRAY_AGG") {
      conditions.push_back(args.at(0) + " IS NOT NULL");
    } else if (name == "APPROX_QUANTILES") {
      // quantile_disc() already skips NULLs, as APPROX_QUANTILES does by default.
    } else if (name == "FIRST_VALUE" || name == "LAST_VALUE" || name == "NTH_VALUE") {
      inner += " IGNORE NULLS";
    } else {
      return Unsupported(scope, name + " IGNORE NULLS");
    }
  }
  if (call.null_handling_modifier() ==
          googlesql::ResolvedNonScalarFunctionCallBase::RESPECT_NULLS &&
      name == "APPROX_QUANTILES") {
    return Unsupported(scope, name + " RESPECT NULLS");
  }
  // ARRAY_CONCAT_AGG skips NULL arrays.
  if (name == "ARRAY_CONCAT_AGG") {
    conditions.push_back(args.at(0) + " IS NOT NULL");
  }
  std::string order;
  if (aggregate != nullptr && !aggregate->order_by_item_list().empty()) {
    const auto items = OrderItems(aggregate->order_by_item_list(), scope, columns);
    if (!items) {
      return std::nullopt;
    }
    order = " ORDER BY " + *items;
  }
  // LIMIT keeps the first elements of the list the aggregate would build; STRING_AGG builds it
  // from its non-NULL values and joins them afterwards.
  std::string limit;
  if (aggregate != nullptr && aggregate->limit() != nullptr) {
    const auto sql = Expression(*aggregate->limit(), scope, columns);
    if (!sql) {
      return std::nullopt;
    }
    limit = *sql;
    if (name == "STRING_AGG") {
      conditions.push_back(args.at(0) + " IS NOT NULL");
      const std::string list = std::string("list(") + (call.distinct() ? "DISTINCT " : "") +
                               args.at(0) + order + ") FILTER (WHERE " + Join(conditions, " AND ") +
                               ")";
      return unhex("array_to_string(list_slice(" + list + ", 1, " + limit + "), " +
                   (args.size() > 1 ? args.at(1) : "','") + ")");
    }
    if (name != "ARRAY_AGG" && name != "ARRAY_CONCAT_AGG") {
      return Unsupported(scope, "aggregate " + name + " with LIMIT");
    }
  }
  const std::string tail =
      (conditions.empty() ? "" : " FILTER (WHERE " + Join(conditions, " AND ") + ")") + over;
  std::string sql = function->second + "(" + inner + order + ")" + tail;
  if (!limit.empty()) {
    sql = "list_slice(" + sql + ", 1, " + limit + ")";
  }
  if (name == "ARRAY_CONCAT_AGG") {
    return "flatten(" + sql + ")";
  }
  // histogram() leaves out NULL, which APPROX_TOP_COUNT counts as a value of its own. Sorting
  // the (count, value) structs descending puts the most frequent first, and no rows give NULL.
  if (!top.empty()) {
    const std::string rows = "count(*)" + tail;
    const std::string nulls = rows + " - count(" + args.at(0) + ")" + tail;
    return "CASE WHEN " + rows + " > 0 THEN list_transform(list_slice(list_sort(list_concat(" +
           "list_transform(map_entries(" + sql +
           "), lambda e: {'count': e.value::BIGINT, 'value': e.key}), CASE WHEN " + nulls +
           " > 0 THEN [{'count': " + nulls + ", 'value': NULL}] END), 'DESC'), 1, " + top +
           "), lambda e: {'value': e.value, 'count': e.count}) END";
  }
  return unhex(sql);
}

std::optional<std::string> FrameBound(const googlesql::ResolvedWindowFrameExpr& bound,
                                      const Scope& scope) {
  switch (bound.boundary_type()) {
    case googlesql::ResolvedWindowFrameExpr::UNBOUNDED_PRECEDING:
      return "UNBOUNDED PRECEDING";
    case googlesql::ResolvedWindowFrameExpr::CURRENT_ROW:
      return "CURRENT ROW";
    case googlesql::ResolvedWindowFrameExpr::UNBOUNDED_FOLLOWING:
      return "UNBOUNDED FOLLOWING";
    case googlesql::ResolvedWindowFrameExpr::OFFSET_PRECEDING:
    case googlesql::ResolvedWindowFrameExpr::OFFSET_FOLLOWING: {
      const auto offset = Expression(*bound.expression(), scope, {});
      if (!offset) {
        return std::nullopt;
      }
      return *offset +
             (bound.boundary_type() == googlesql::ResolvedWindowFrameExpr::OFFSET_PRECEDING
                  ? " PRECEDING"
                  : " FOLLOWING");
    }
    default:
      return std::nullopt;
  }
}

std::optional<std::string> Window(const googlesql::ResolvedAnalyticFunctionGroup& group,
                                  const googlesql::ResolvedWindowFrame* frame, const Scope& scope,
                                  const Columns& columns) {
  std::vector<std::string> window;
  if (const auto* partition = group.partition_by(); partition != nullptr) {
    if (!partition->hint_list().empty() || !partition->collation_list().empty()) {
      return std::nullopt;
    }
    std::vector<std::string> keys;
    for (const auto& ref : partition->partition_by_list()) {
      const auto key = Expression(*ref, scope, columns);
      if (!key) {
        return std::nullopt;
      }
      keys.push_back(*key);
    }
    window.push_back("PARTITION BY " + Join(keys, ", "));
  }
  if (const auto* ordering = group.order_by(); ordering != nullptr) {
    if (!ordering->hint_list().empty()) {
      return std::nullopt;
    }
    const auto order = OrderItems(ordering->order_by_item_list(), scope, columns);
    if (!order) {
      return std::nullopt;
    }
    window.push_back("ORDER BY " + *order);
  }
  if (frame != nullptr) {
    const auto start = FrameBound(*frame->start_expr(), scope);
    const auto end = FrameBound(*frame->end_expr(), scope);
    if (!start || !end) {
      return std::nullopt;
    }
    window.push_back(std::string(frame->frame_unit() == googlesql::ResolvedWindowFrame::ROWS
                                     ? "ROWS"
                                     : "RANGE") +
                     " BETWEEN " + *start + " AND " + *end);
  }
  return " OVER (" + Join(window, " ") + ")";
}

// Spells one element of a grouping set list over the group by keys in `keys`.
std::optional<std::string> GroupingSet(const googlesql::ResolvedGroupingSetBase& set,
                                       const Columns& keys) {
  const auto references =
      [&](const std::vector<std::unique_ptr<const googlesql::ResolvedColumnRef>>& refs)
      -> std::optional<std::string> {
    std::vector<std::string> sql;
    for (const auto& ref : refs) {
      const auto key = keys.find(ref->column().column_id());
      if (key == keys.end()) {
        return std::nullopt;
      }
      sql.push_back(key->second);
    }
    return "(" + Join(sql, ", ") + ")";
  };
  const auto multi_columns =
      [&](const std::vector<std::unique_ptr<const googlesql::ResolvedGroupingSetMultiColumn>>& list)
      -> std::optional<std::string> {
    std::vector<std::string> sql;
    for (const auto& columns : list) {
      const auto item = references(columns->column_list());
      if (!item) {
        return std::nullopt;
      }
      sql.push_back(*item);
    }
    return Join(sql, ", ");
  };
  const auto elements =
      [&](const std::vector<std::unique_ptr<const googlesql::ResolvedGroupingSetBase>>& list)
      -> std::optional<std::string> {
    std::vector<std::string> sql;
    for (const auto& element : list) {
      const auto item = GroupingSet(*element, keys);
      if (!item) {
        return std::nullopt;
      }
      sql.push_back(*item);
    }
    return Join(sql, ", ");
  };
  if (set.Is<googlesql::ResolvedGroupingSet>()) {
    return references(set.GetAs<googlesql::ResolvedGroupingSet>()->group_by_column_list());
  }
  std::optional<std::string> inner;
  if (set.Is<googlesql::ResolvedRollup>()) {
    inner = multi_columns(set.GetAs<googlesql::ResolvedRollup>()->rollup_column_list());
    return inner ? std::optional<std::string>("ROLLUP(" + *inner + ")") : std::nullopt;
  }
  if (set.Is<googlesql::ResolvedCube>()) {
    inner = multi_columns(set.GetAs<googlesql::ResolvedCube>()->cube_column_list());
    return inner ? std::optional<std::string>("CUBE(" + *inner + ")") : std::nullopt;
  }
  if (set.Is<googlesql::ResolvedGroupingSetList>()) {
    inner = elements(set.GetAs<googlesql::ResolvedGroupingSetList>()->elem_list());
    return inner ? std::optional<std::string>("GROUPING SETS (" + *inner + ")") : std::nullopt;
  }
  if (set.Is<googlesql::ResolvedGroupingSetProduct>()) {
    // DuckDB reads a parenthesized list in GROUP BY a, (b, c) as a row, so each factor of the
    // product is spelled as a grouping set list of its own.
    std::vector<std::string> factors;
    for (const auto& factor : set.GetAs<googlesql::ResolvedGroupingSetProduct>()->input_list()) {
      const auto item = GroupingSet(*factor, keys);
      if (!item) {
        return std::nullopt;
      }
      factors.push_back("GROUPING SETS (" + *item + ")");
    }
    return Join(factors, ", ");
  }
  return std::nullopt;
}

}  // namespace

std::optional<Relation> AggregateScan(const googlesql::ResolvedAggregateScan& aggregate,
                                      const Scope& scope) {
  if (!aggregate.collation_list().empty()) {
    return Unsupported(scope, "GROUP BY with collation");
  }
  auto input = Scan(*aggregate.input_scan(), scope);
  if (!input) {
    return std::nullopt;
  }
  Relation result;
  std::vector<std::string> projections;
  std::vector<std::string> keys;
  // The rows each HAVING MAX/MIN modifier keeps, keyed by the aggregate call it modifies.
  std::map<const googlesql::ResolvedAggregateFunctionCall*, std::string> havings;
  // GROUPING() without grouping sets still needs them: it is 0 over the one plain grouping.
  if (!aggregate.grouping_set_list().empty() || !aggregate.grouping_call_list().empty()) {
    // Grouping sets name their keys, so compute the keys once below the aggregation and group
    // by the column names rather than by position.
    std::vector<std::string> inner;
    Columns columns;
    for (const auto& [id, sql] : input->columns) {
      inner.push_back(sql + " AS " + ColumnName(id));
      columns.emplace(id, "q." + ColumnName(id));
    }
    Columns key_columns;
    for (const auto& key : aggregate.group_by_list()) {
      const auto sql = Expression(*key->expr(), scope, input->columns);
      if (!sql) {
        return std::nullopt;
      }
      const int id = key->column().column_id();
      inner.push_back(*sql + " AS " + ColumnName(id));
      key_columns.emplace(id, "q." + ColumnName(id));
      projections.push_back("q." + ColumnName(id) + " AS " + ColumnName(id));
      result.columns.emplace(id, "q." + ColumnName(id));
    }
    std::vector<std::string> all_keys;
    for (const auto& [id, sql] : key_columns) {
      all_keys.push_back(sql);
    }
    for (const auto& set : aggregate.grouping_set_list()) {
      const auto sql = GroupingSet(*set, key_columns);
      if (!sql) {
        return Unsupported(scope, "grouping set");
      }
      keys.push_back(*sql);
    }
    if (keys.empty()) {
      keys.push_back("(" + Join(all_keys, ", ") + ")");
    }
    // A product stands for GROUP BY a, ROLLUP(b), which is its own spelling.
    if (keys.size() > 1 || aggregate.grouping_set_list().empty() ||
        !aggregate.grouping_set_list(0)->Is<googlesql::ResolvedGroupingSetProduct>()) {
      keys = {"GROUPING SETS (" + Join(keys, ", ") + ")"};
    }
    for (const auto& call : aggregate.grouping_call_list()) {
      const auto key = key_columns.find(call->group_by_column()->column().column_id());
      if (key == key_columns.end()) {
        return std::nullopt;
      }
      const int id = call->output_column().column_id();
      projections.push_back("CAST(GROUPING(" + key->second + ") AS BIGINT) AS " + ColumnName(id));
      result.columns.emplace(id, "q." + ColumnName(id));
    }
    input = Relation{
        .sql = "SELECT " + (inner.empty() ? "1 AS _unit" : Join(inner, ", ")) + input->From(),
        .columns = columns};
  } else {
    // HAVING MAX/MIN keeps the rows whose expression reaches the group's maximum or minimum,
    // which a window over the group computes alongside the input.
    std::vector<std::string> partition;
    for (const auto& key : aggregate.group_by_list()) {
      const auto sql = Expression(*key->expr(), scope, input->columns);
      if (!sql) {
        return std::nullopt;
      }
      partition.push_back(*sql);
    }
    std::vector<std::string> inner;
    Columns columns;
    for (const auto& [id, sql] : input->columns) {
      inner.push_back(sql + " AS " + ColumnName(id));
      columns.emplace(id, "q." + ColumnName(id));
    }
    for (const auto& computed : aggregate.aggregate_list()) {
      const auto* call = computed->expr()->GetAs<googlesql::ResolvedAggregateFunctionCall>();
      if (!computed->expr()->Is<googlesql::ResolvedAggregateFunctionCall>() ||
          call->having_modifier() == nullptr) {
        continue;
      }
      const auto& modifier = *call->having_modifier();
      const auto window = Expression(*modifier.having_expr(), scope, input->columns);
      const auto row = Expression(*modifier.having_expr(), scope, columns);
      if (!window || !row) {
        return std::nullopt;
      }
      const std::string name = "_h" + std::to_string(havings.size());
      inner.push_back(
          std::string(modifier.kind() == googlesql::ResolvedAggregateHavingModifier::MAX ? "max("
                                                                                         : "min(") +
          *window + ") OVER (" +
          (partition.empty() ? "" : "PARTITION BY " + Join(partition, ", ")) + ") AS " + name);
      havings.emplace(call, *row + " = q." + name);
    }
    if (!havings.empty()) {
      input = Relation{.sql = "SELECT " + Join(inner, ", ") + input->From(), .columns = columns};
    }
    for (const auto& key : aggregate.group_by_list()) {
      const auto sql = Expression(*key->expr(), scope, input->columns);
      if (!sql) {
        return std::nullopt;
      }
      const int id = key->column().column_id();
      projections.push_back(*sql + " AS " + ColumnName(id));
      keys.push_back(std::to_string(projections.size()));
      result.columns.emplace(id, "q." + ColumnName(id));
    }
  }
  for (const auto& computed : aggregate.aggregate_list()) {
    const auto* call = computed->expr()->GetAs<googlesql::ResolvedAggregateFunctionCall>();
    const auto type = SqlType(computed->expr()->type());
    if (!computed->expr()->Is<googlesql::ResolvedAggregateFunctionCall>() || !type) {
      return std::nullopt;
    }
    const auto having = havings.find(call);
    const auto sql = NonScalarCall(*call, call, "", scope, input->columns,
                                   having == havings.end() ? "" : having->second);
    if (!sql) {
      return std::nullopt;
    }
    const int id = computed->column().column_id();
    projections.push_back("CAST(" + *sql + " AS " + *type + ") AS " + ColumnName(id));
    result.columns.emplace(id, "q." + ColumnName(id));
  }
  if (projections.empty()) {
    return std::nullopt;
  }
  result.sql = "SELECT " + Join(projections, ", ") + input->From() +
               (keys.empty() ? "" : " GROUP BY " + Join(keys, ", "));
  return result;
}

std::optional<Relation> AnalyticScan(const googlesql::ResolvedAnalyticScan& analytic,
                                     const Scope& scope) {
  auto result = Scan(*analytic.input_scan(), scope);
  if (!result) {
    return std::nullopt;
  }
  const Columns input_columns = result->columns;
  std::vector<std::string> projections = {"q.*"};
  for (const auto& group : analytic.function_group_list()) {
    for (const auto& computed : group->analytic_function_list()) {
      if (!computed->expr()->Is<googlesql::ResolvedAnalyticFunctionCall>()) {
        return std::nullopt;
      }
      const auto* call = computed->expr()->GetAs<googlesql::ResolvedAnalyticFunctionCall>();
      const auto type = SqlType(call->type());
      const auto over = Window(*group, call->window_frame(), scope, input_columns);
      if (!type || !over) {
        return std::nullopt;
      }
      const auto sql = NonScalarCall(*call, nullptr, *over, scope, input_columns);
      if (!sql) {
        return std::nullopt;
      }
      const int id = computed->column().column_id();
      projections.push_back("CAST(" + *sql + " AS " + *type + ") AS " + ColumnName(id));
      result->columns.emplace(id, "q." + ColumnName(id));
    }
  }
  result->sql = "SELECT " + Join(projections, ", ") + result->From();
  return result;
}

}  // namespace bigquery_emulator_duckdb::translator
