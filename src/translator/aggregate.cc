#include "src/translator/aggregate.h"

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "googlesql/analyzer/expr_resolver_helper.h"
#include "googlesql/public/function.h"
#include "googlesql/public/type.h"
#include "googlesql/resolved_ast/resolved_ast.h"
#include "src/duckdb_sql.h"
#include "src/function_catalog/catalog.h"
#include "src/function_catalog/rules.h"
#include "src/language_options.h"
#include "src/translator/context.h"
#include "src/translator/expression.h"
#include "src/translator/function.h"
#include "src/translator/scan.h"
#include "src/type_mapping.h"

namespace bigquery_emulator_duckdb::translator {

using function_catalog::AggregateArguments;
using function_catalog::AggregateOrder;
using function_catalog::AggregateRule;
using function_catalog::Condition;
using function_catalog::FunctionEntry;
using function_catalog::Translation;
using function_catalog::TypeUse;

namespace {

// Aggregate calls (with `aggregate` set) and analytic calls (with a non-empty `over`). An
// aggregate's HAVING MAX/MIN modifier is the caller's to compute, as the rows `having` selects.
std::optional<std::string> NonScalarCall(const googlesql::ResolvedNonScalarFunctionCallBase& call,
                                         const googlesql::ResolvedAggregateFunctionCall* aggregate,
                                         const std::string& over, const Scope& scope,
                                         const Columns& columns, const std::string& having = "",
                                         std::vector<std::string>* intermediates = nullptr) {
  const auto& functions = scope.context.functions;
  const std::string name = ToUpperAscii(call.function()->FullName(/*include_group=*/false));
  if (HasGraph(call.type()) || std::ranges::any_of(call.argument_list(), [](const auto& argument) {
        return HasGraph(argument->type());
      })) {
    return Unsupported(scope, "aggregate or analytic function " + name + " with graph values");
  }
  if (!call.generic_argument_list().empty() || !call.hint_list().empty() ||
      !call.collation_list().empty() ||
      call.error_mode() != googlesql::ResolvedFunctionCallBase::DEFAULT_ERROR_MODE ||
      !IsBuiltinFunction(*call.function()) || call.where_expr() != nullptr) {
    return Unsupported(scope, "aggregate or analytic function " + name + " with modifiers");
  }
  if (aggregate != nullptr &&
      ((aggregate->having_modifier() != nullptr && having.empty()) ||
       !aggregate->group_by_list().empty() || !aggregate->group_by_aggregate_list().empty() ||
       aggregate->having_expr() != nullptr)) {
    return Unsupported(scope, "aggregate " + name + " with HAVING or GROUP BY");
  }
  const FunctionEntry* entry = functions.Find(name);
  if (entry == nullptr ||
      (entry->translation != Translation::kAggregate &&
       entry->translation != Translation::kAnalytic) ||
      (entry->translation == Translation::kAnalytic && over.empty())) {
    return Unsupported(scope, "aggregate or analytic function " + name);
  }
  if ((name == "LAG" || name == "LEAD") && call.argument_list_size() == 3) {
    const auto constant = googlesql::IsConstantExpression(call.argument_list(2));
    if (!constant.ok()) {
      return Unsupported(scope, name + " default expression");
    }
    if (!*constant) {
      throw std::invalid_argument("The third argument (the default expression) to " + name +
                                  " must be constant");
    }
  }
  const auto type_of = [&](std::size_t argument) {
    return argument < call.argument_list().size()
               ? call.argument_list().at(argument)->type()->kind()
               : googlesql::TYPE_UNKNOWN;
  };
  const auto rule = std::ranges::find_if(entry->aggregates, [&](const AggregateRule& candidate) {
    return (candidate.type == googlesql::TYPE_UNKNOWN || candidate.type == type_of(0)) &&
           (candidate.distinct != AggregateRule::Distinct::kRequired || call.distinct()) &&
           std::ranges::all_of(candidate.conditions, [&](const Condition& condition) {
             return std::ranges::find(condition.types, type_of(condition.argument - 1)) !=
                    condition.types.end();
           });
  });
  // GoogleSQL accepts INTERVAL values in these, but BigQuery rejects them after analysis.
  static const std::map<std::string_view, std::string_view> interval_rejections = {
      {"APPROX_COUNT_DISTINCT", "Unsupported type INTERVAL for hll_count.init."},
      {"APPROX_QUANTILES", "Unsupported type INTERVAL for aggregate function approx_quantiles"},
      {"APPROX_TOP_COUNT", "Unsupported type INTERVAL for approx_top_count_shard."},
      {"APPROX_TOP_SUM", "Unsupported type INTERVAL for approx_top_sum_shard."},
  };
  if (const auto rejection = interval_rejections.find(name);
      rejection != interval_rejections.end() && type_of(0) == googlesql::TYPE_INTERVAL) {
    throw std::invalid_argument(std::string(rejection->second));
  }
  // BigQuery rejects ARRAY_AGG over INTERVAL values ordered by themselves alone after analysis,
  // as it must with DISTINCT, but orders them by other keys, also with themselves.
  if (name == "ARRAY_AGG" && type_of(0) == googlesql::TYPE_INTERVAL && aggregate != nullptr &&
      !aggregate->order_by_item_list().empty()) {
    const auto& items = aggregate->order_by_item_list();
    if (call.distinct() ||
        (items.size() == 1 && call.argument_list(0)->Is<googlesql::ResolvedColumnRef>() &&
         call.argument_list(0)->GetAs<googlesql::ResolvedColumnRef>()->column() ==
             items.at(0)->column_ref()->column())) {
      throw std::invalid_argument("Unsupported type INTERVAL for array_agg.");
    }
  }
  // Only an INTERVAL overload takes INTERVAL values, and one that carries them takes them in
  // other values; the others would add or compare them as DuckDB does.
  if ((rule == entry->aggregates.end() ||
       (rule->type != googlesql::TYPE_INTERVAL && (!rule->carries || call.distinct()))) &&
      (HasInterval(call.type()) ||
       std::ranges::any_of(call.argument_list(),
                           [](const auto& argument) { return HasInterval(argument->type()); }))) {
    return Unsupported(scope, "aggregate or analytic function " + name + " with INTERVAL");
  }
  if (HasEmptyStruct(call.type()) ||
      std::ranges::any_of(call.argument_list(),
                          [](const auto& argument) { return HasEmptyStruct(argument->type()); })) {
    return Unsupported(scope, "aggregate or analytic function " + name + " with STRUCT<>");
  }
  // BigQuery rejects these over RANGE values although GoogleSQL accepts them, naming the
  // functions it implements them with.
  static const std::map<std::string_view, std::string_view> range_rejections = {
      {"APPROX_COUNT_DISTINCT", "hll_count.init"},
      {"APPROX_TOP_COUNT", "approx_top_count_shard"},
      {"APPROX_TOP_SUM", "approx_top_sum_shard"},
  };
  if (const auto rejection = range_rejections.find(name);
      rejection != range_rejections.end() && !call.argument_list().empty() &&
      call.argument_list(0)->type()->IsRange()) {
    throw std::invalid_argument("Unsupported type RANGE for " + std::string(rejection->second) +
                                ".");
  }
  if ((HasRange(call.type()) ||
       std::ranges::any_of(call.argument_list(),
                           [](const auto& argument) { return HasRange(argument->type()); })) &&
      !functions.HasTypeUse(name, TypeUse::kAggregatesRange)) {
    return Unsupported(scope, "aggregate or analytic function " + name + " with RANGE");
  }
  if (!functions.HasTypeUse(name, TypeUse::kBigNumeric) && InvolvesBigNumeric(call)) {
    return Unsupported(scope, "function " + name + " with BIGNUMERIC");
  }
  if (rule == entry->aggregates.end()) {
    return Unsupported(scope, "aggregate or analytic function " + name);
  }
  std::vector<std::string> arguments;
  for (const auto& argument : call.argument_list()) {
    const auto sql = Expression(*argument, scope, columns);
    if (!sql) {
      return std::nullopt;
    }
    arguments.push_back(*sql);
  }
  const std::vector<std::string> args = AggregateArguments(*rule, arguments);
  const auto finish = [&](const std::string& sql, const std::string& tail) {
    if (rule->finish_after_scan && intermediates != nullptr) {
      const std::string column = scope.context.FreshName("_agg");
      intermediates->push_back(sql + " AS " + column);
      return rule->finish("q." + column, arguments, "");
    }
    return rule->finish == nullptr ? sql : rule->finish(sql, arguments, tail);
  };
  if (call.distinct() && rule->distinct == AggregateRule::Distinct::kUnsupported) {
    return Unsupported(scope, name + "(DISTINCT ...)");
  }
  std::string inner = Join(args, ", ");
  if ((call.distinct() && rule->distinct != AggregateRule::Distinct::kRequired) ||
      rule->distinct == AggregateRule::Distinct::kAlways) {
    inner = "DISTINCT " + inner;
  }
  std::vector<std::string> conditions;
  if (!having.empty()) {
    conditions.push_back(having);
  }
  switch (call.null_handling_modifier()) {
    case googlesql::ResolvedNonScalarFunctionCallBase::IGNORE_NULLS:
      if (rule->nulls == AggregateRule::Nulls::kFilter ||
          rule->nulls == AggregateRule::Nulls::kFilterUnlessRespected) {
        conditions.push_back(args.at(0) + " IS NOT NULL");
      } else if (rule->nulls == AggregateRule::Nulls::kModifier) {
        inner += " IGNORE NULLS";
      } else if (rule->nulls != AggregateRule::Nulls::kIgnore) {
        return Unsupported(scope, name + " IGNORE NULLS");
      }
      break;
    case googlesql::ResolvedNonScalarFunctionCallBase::RESPECT_NULLS:
      if (rule->nulls == AggregateRule::Nulls::kIgnore) {
        return Unsupported(scope, name + " RESPECT NULLS");
      }
      break;
    default:
      if (rule->nulls == AggregateRule::Nulls::kFilterUnlessRespected) {
        conditions.push_back(args.at(0) + " IS NOT NULL");
      }
      break;
  }
  if (rule->skip_nulls) {
    conditions.push_back(args.at(0) + " IS NOT NULL");
  }
  std::string order;
  if (aggregate != nullptr && !aggregate->order_by_item_list().empty()) {
    const auto items = OrderItems(aggregate->order_by_item_list(), scope, columns);
    if (!items) {
      return std::nullopt;
    }
    order = " ORDER BY " + *items;
  } else if (const std::string items = AggregateOrder(*rule, arguments); !items.empty()) {
    order = " ORDER BY " + items;
  }
  // LIMIT keeps the first elements of the list the aggregate would build; STRING_AGG builds it
  // from its non-NULL values and joins them afterwards.
  std::string limit;
  if (aggregate != nullptr && aggregate->limit() != nullptr) {
    const auto sql = Expression(*aggregate->limit(), scope, columns);
    if (!sql) {
      return std::nullopt;
    }
    // A zero-row aggregate is NULL, not the empty list/string a zero-length slice yields.
    limit = "nullif(" + *sql + ", 0)";
    if (rule->limit == AggregateRule::Limit::kJoin) {
      conditions.push_back(args.at(0) + " IS NOT NULL");
      const std::string list = std::string("list(") + (call.distinct() ? "DISTINCT " : "") +
                               args.at(0) + order + ") FILTER (WHERE " + Join(conditions, " AND ") +
                               ")";
      return finish("list_aggr(list_slice(" + list + ", 1, " + limit + "), 'string_agg', " +
                        (args.size() > 1 ? args.at(1) : "','") + ")",
                    over);
    }
    if (rule->limit != AggregateRule::Limit::kSlice &&
        rule->limit != AggregateRule::Limit::kSliceFinished) {
      return Unsupported(scope, "aggregate " + name + " with LIMIT");
    }
  }
  const std::string tail =
      (conditions.empty() ? "" : " FILTER (WHERE " + Join(conditions, " AND ") + ")") + over;
  const std::string sql = std::string(rule->function) + "(" + inner + order + ")" + tail;
  if (limit.empty()) {
    return finish(sql, tail);
  }
  if (rule->limit == AggregateRule::Limit::kSliceFinished) {
    return "list_slice(" + finish(sql, tail) + ", 1, " + limit + ")";
  }
  return finish("list_slice(" + sql + ", 1, " + limit + ")", tail);
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
      if (HasGraph(ref->type())) {
        return Unsupported(scope, "PARTITION BY with graph values");
      }
      if (ref->type()->IsStruct()) {
        throw std::invalid_argument("Partitioning by expressions of type STRUCT is unsupported");
      }
      if (HasInterval(ref->type()) && !ref->type()->IsInterval()) {
        return Unsupported(scope, "PARTITION BY with INTERVAL");
      }
      const auto key = Expression(*ref, scope, columns);
      if (!key) {
        return std::nullopt;
      }
      keys.push_back(ref->type()->IsInterval() ? IntervalKey(*key) : *key);
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

// Lists the groupings one element of a grouping set list stands for, each as the column ids of
// the keys it groups by.
std::optional<std::vector<std::set<int>>> ExpandGroupingSet(
    const googlesql::ResolvedGroupingSetBase& set) {
  const auto ids =
      [](const std::vector<std::unique_ptr<const googlesql::ResolvedColumnRef>>& refs) {
        std::set<int> result;
        std::ranges::transform(refs, std::inserter(result, result.end()),
                               [](const auto& ref) { return ref->column().column_id(); });
        return result;
      };
  const auto multi_columns =
      [&](const std::vector<std::unique_ptr<const googlesql::ResolvedGroupingSetMultiColumn>>&
              list) {
        std::vector<std::set<int>> result;
        std::ranges::transform(list, std::back_inserter(result),
                               [&](const auto& columns) { return ids(columns->column_list()); });
        return result;
      };
  if (set.Is<googlesql::ResolvedGroupingSet>()) {
    return std::vector{ids(set.GetAs<googlesql::ResolvedGroupingSet>()->group_by_column_list())};
  }
  if (set.Is<googlesql::ResolvedRollup>()) {
    const auto columns =
        multi_columns(set.GetAs<googlesql::ResolvedRollup>()->rollup_column_list());
    std::vector<std::set<int>> result;
    for (std::size_t size = columns.size() + 1; size-- > 0;) {
      std::set<int> keys;
      for (std::size_t i = 0; i < size; ++i) {
        keys.insert(columns.at(i).begin(), columns.at(i).end());
      }
      result.push_back(keys);
    }
    return result;
  }
  if (set.Is<googlesql::ResolvedCube>()) {
    const auto columns = multi_columns(set.GetAs<googlesql::ResolvedCube>()->cube_column_list());
    std::vector<std::set<int>> result = {{}};
    for (const auto& column : columns) {
      const std::size_t size = result.size();
      for (std::size_t i = 0; i < size; ++i) {
        std::set<int> keys = result.at(i);
        keys.insert(column.begin(), column.end());
        result.push_back(keys);
      }
    }
    return result;
  }
  if (set.Is<googlesql::ResolvedGroupingSetList>()) {
    std::vector<std::set<int>> result;
    for (const auto& element : set.GetAs<googlesql::ResolvedGroupingSetList>()->elem_list()) {
      const auto sets = ExpandGroupingSet(*element);
      if (!sets) {
        return std::nullopt;
      }
      result.insert(result.end(), sets->begin(), sets->end());
    }
    return result;
  }
  if (set.Is<googlesql::ResolvedGroupingSetProduct>()) {
    std::vector<std::set<int>> result = {{}};
    for (const auto& factor : set.GetAs<googlesql::ResolvedGroupingSetProduct>()->input_list()) {
      const auto sets = ExpandGroupingSet(*factor);
      if (!sets) {
        return std::nullopt;
      }
      std::vector<std::set<int>> product;
      for (const auto& left : result) {
        for (const auto& right : *sets) {
          std::set<int> keys = left;
          keys.insert(right.begin(), right.end());
          product.push_back(keys);
        }
      }
      result = std::move(product);
    }
    return result;
  }
  return std::nullopt;
}

// Project finishes only after the aggregates have been computed, and expose only resolved
// columns so intermediate lists cannot collide with later scans or escape into results.
void ApplyFinishes(Relation& result, const std::map<int, std::string>& finishes) {
  if (finishes.empty()) {
    return;
  }
  std::vector<std::string> projections;
  for (const auto& [id, column] : result.columns) {
    const auto finish = finishes.find(id);
    projections.push_back((finish == finishes.end() ? column : finish->second) + " AS " +
                          ColumnName(id));
  }
  result.sql = "SELECT " + Join(projections, ", ") + result.From();
}

// GoogleSQL aggregates enclosing columns over the subquery's rows, but DuckDB evaluates an
// aggregate whose arguments read only enclosing columns in the enclosing query. Repeating those
// columns in the input makes DuckDB bind them there, to the same values; the scope returned
// names them for the aggregate calls.
Scope WithCorrelatedColumns(const googlesql::ResolvedAggregateScan& aggregate, const Scope& scope,
                            Relation& input) {
  Scope local = scope;
  std::vector<std::string> projections = {"q.*"};
  for (const auto& computed : aggregate.aggregate_list()) {
    std::vector<const googlesql::ResolvedNode*> refs;
    computed->expr()->GetDescendantsWithKinds({googlesql::RESOLVED_COLUMN_REF}, &refs);
    for (const auto* node : refs) {
      const auto* ref = node->GetAs<googlesql::ResolvedColumnRef>();
      const int id = ref->column().column_id();
      const auto outer = local.outer.find(id);
      if (!ref->is_correlated() || outer == local.outer.end() || outer->second != ColumnName(id)) {
        continue;
      }
      outer->second = QuoteIdentifier("_l" + std::to_string(id));
      projections.push_back(ColumnName(id) + " AS " + outer->second);
    }
  }
  if (projections.size() > 1) {
    input.sql = "SELECT " + Join(projections, ", ") + input.From();
  }
  return local;
}

}  // namespace

std::optional<Relation> AggregateScan(const googlesql::ResolvedAggregateScan& aggregate,
                                      const Scope& scope) {
  if (std::ranges::any_of(aggregate.group_by_list(),
                          [](const auto& key) { return HasGraph(key->expr()->type()); })) {
    return Unsupported(scope, "GROUP BY with graph values");
  }
  // Keys with INTERVAL values group by their lengths, and return the first of equal values in input
  // order, as BigQuery does.
  const bool grouping_sets =
      !aggregate.grouping_set_list().empty() || !aggregate.grouping_call_list().empty();
  if (!aggregate.collation_list().empty()) {
    return Unsupported(scope, "GROUP BY with collation");
  }
  auto input = Scan(*aggregate.input_scan(), scope);
  if (!input) {
    return std::nullopt;
  }
  Relation result;
  std::vector<std::string> projections;
  std::vector<std::string> intermediates;
  std::map<int, std::string> finishes;
  std::vector<std::string> keys;
  // The rows each HAVING MAX/MIN modifier keeps, keyed by the aggregate call it modifies.
  std::map<const googlesql::ResolvedAggregateFunctionCall*, std::string> havings;
  // With grouping sets, the keys with their types and the GROUPING() calls with their keys, which
  // a grouping that leaves out a key spells as NULL and 1.
  std::vector<std::pair<int, std::string>> key_projections;
  std::vector<std::pair<int, int>> grouping_projections;
  Columns key_columns;
  // GROUPING() without grouping sets still needs them: it is 0 over the one plain grouping.
  if (grouping_sets) {
    // Grouping sets name their keys, so compute the keys once below the aggregation and group
    // by the column names rather than by position.
    std::vector<std::string> inner;
    Columns columns;
    for (const auto& [id, sql] : input->columns) {
      inner.push_back(sql + " AS " + ColumnName(id));
      columns.emplace(id, "q." + ColumnName(id));
    }
    // BigQuery returns the first of equal values in the whole input, also in the groups of later
    // rows, so a key with INTERVAL values becomes that value before the grouping. Values with
    // different lengths differ in DuckDB too, so DuckDB groups them as BigQuery does.
    std::vector<std::string> firsts;
    for (const auto& key : aggregate.group_by_list()) {
      const auto sql = Expression(*key->expr(), scope, input->columns);
      if (!sql) {
        return std::nullopt;
      }
      const int id = key->column().column_id();
      inner.push_back(*sql + " AS " + ColumnName(id));
      if (const googlesql::Type* type = key->expr()->type(); HasInterval(type)) {
        const std::string column = "q." + ColumnName(id);
        firsts.push_back("first_value(" + column + ") OVER (PARTITION BY " +
                         IntervalKey(type, column) + " ORDER BY q._rn) AS " + ColumnName(id));
      }
      key_columns.emplace(id, "q." + ColumnName(id));
      const auto type = DuckDbType(key->expr()->type());
      if (!type) {
        return std::nullopt;
      }
      key_projections.emplace_back(id, *type);
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
      grouping_projections.emplace_back(id, key->first);
      projections.push_back("CAST(GROUPING(" + key->second + ") AS BIGINT) AS " + ColumnName(id));
      result.columns.emplace(id, "q." + ColumnName(id));
    }
    std::string sql = "SELECT " + (inner.empty() ? "1 AS _unit" : Join(inner, ", "));
    if (!firsts.empty()) {
      sql = "SELECT q.* REPLACE (" + Join(firsts, ", ") + ") FROM (" + sql +
            ", row_number() OVER () AS _rn" + input->From() + ") AS q";
    } else {
      sql += input->From();
    }
    input = Relation{.sql = sql, .columns = columns};
  } else {
    // HAVING MAX/MIN keeps the rows whose expression reaches the group's maximum or minimum,
    // which a window over the group computes alongside the input; INTERVAL values compare by
    // their keys.
    std::vector<std::string> partition;
    std::vector<std::string> partition_values;
    for (const auto& key : aggregate.group_by_list()) {
      const auto sql = Expression(*key->expr(), scope, input->columns);
      if (!sql) {
        return std::nullopt;
      }
      const std::string name = "_partition" + std::to_string(partition.size());
      partition_values.push_back(IntervalKey(key->expr()->type(), *sql) + " AS " + name);
      partition.push_back("q." + name);
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
      const googlesql::Type* type = modifier.having_expr()->type();
      const auto window = Expression(*modifier.having_expr(), scope, input->columns);
      const auto row = Expression(*modifier.having_expr(), scope, columns);
      if (!window || !row) {
        return std::nullopt;
      }
      const std::string name = "_h" + std::to_string(havings.size());
      inner.push_back(
          std::string(modifier.kind() == googlesql::ResolvedAggregateHavingModifier::MAX ? "max("
                                                                                         : "min(") +
          IntervalKey(type, *window) + ") OVER (" +
          (partition.empty() ? "" : "PARTITION BY " + Join(partition, ", ")) + ") AS " + name);
      havings.emplace(call, IntervalKey(type, *row) + " = q." + name);
    }
    if (!havings.empty()) {
      // A guarded expression can contain error(), which DuckDB v2 considers volatile and
      // forbids directly in a window's PARTITION BY. Bind the keys below the window instead.
      if (!partition_values.empty()) {
        input->sql = "SELECT q.*, " + Join(partition_values, ", ") + input->From();
      }
      input = Relation{.sql = "SELECT " + Join(inner, ", ") + input->From(), .columns = columns};
    }
    for (const auto& key : aggregate.group_by_list()) {
      const auto sql = Expression(*key->expr(), scope, input->columns);
      if (!sql) {
        return std::nullopt;
      }
      const int id = key->column().column_id();
      if (const googlesql::Type* type = key->expr()->type(); HasInterval(type)) {
        projections.push_back("first(" + *sql + ") AS " + ColumnName(id));
        keys.push_back(IntervalKey(type, *sql));
      } else {
        projections.push_back(*sql + " AS " + ColumnName(id));
        keys.push_back(std::to_string(projections.size()));
      }
      result.columns.emplace(id, "q." + ColumnName(id));
    }
  }
  const Scope aggregate_scope = WithCorrelatedColumns(aggregate, scope, *input);
  // DuckDB cannot tell an aggregate's rows which grouping they belong to, so HAVING MAX/MIN with
  // grouping sets computes each grouping on its own, with windows over its groups: each
  // modifier's aggregate, with the column it is computed as.
  std::vector<std::pair<std::string, std::string>> windows;
  if (grouping_sets) {
    for (const auto& computed : aggregate.aggregate_list()) {
      const auto* call = computed->expr()->GetAs<googlesql::ResolvedAggregateFunctionCall>();
      if (!computed->expr()->Is<googlesql::ResolvedAggregateFunctionCall>() ||
          call->having_modifier() == nullptr) {
        continue;
      }
      const auto& modifier = *call->having_modifier();
      auto sql = Expression(*modifier.having_expr(), aggregate_scope, input->columns);
      if (!sql) {
        return std::nullopt;
      }
      sql = IntervalKey(modifier.having_expr()->type(), *sql);
      const std::string name = "_h" + std::to_string(havings.size());
      windows.emplace_back(
          std::string(modifier.kind() == googlesql::ResolvedAggregateHavingModifier::MAX ? "max("
                                                                                         : "min(") +
              *sql + ")",
          name);
      havings.emplace(call, *sql + " = q." + name);
    }
  }
  for (const auto& computed : aggregate.aggregate_list()) {
    const auto* call = computed->expr()->GetAs<googlesql::ResolvedAggregateFunctionCall>();
    const auto type = DuckDbType(computed->expr()->type());
    if (!computed->expr()->Is<googlesql::ResolvedAggregateFunctionCall>() || !type) {
      return std::nullopt;
    }
    const auto having = havings.find(call);
    const std::size_t previous = intermediates.size();
    const auto sql = NonScalarCall(*call, call, "", aggregate_scope, input->columns,
                                   having == havings.end() ? "" : having->second, &intermediates);
    if (!sql) {
      return std::nullopt;
    }
    const int id = computed->column().column_id();
    const std::string cast = "CAST(" + *sql + " AS " + *type + ")";
    if (intermediates.size() == previous) {
      projections.push_back(cast + " AS " + ColumnName(id));
    } else {
      finishes.emplace(id, cast);
    }
    result.columns.emplace(id, "q." + ColumnName(id));
  }
  projections.insert(projections.end(), intermediates.begin(), intermediates.end());
  if (projections.empty()) {
    return std::nullopt;
  }
  if (windows.empty()) {
    result.sql = "SELECT " + Join(projections, ", ") + input->From() +
                 (keys.empty() ? "" : " GROUP BY " + Join(keys, ", "));
  } else {
    std::vector<std::set<int>> groupings;
    for (const auto& set : aggregate.grouping_set_list()) {
      const auto sets = ExpandGroupingSet(*set);
      if (!sets) {
        return Unsupported(scope, "grouping set");
      }
      groupings.insert(groupings.end(), sets->begin(), sets->end());
    }
    if (aggregate.grouping_set_list().empty()) {
      std::set<int> all;
      for (const auto& [id, type] : key_projections) {
        all.insert(id);
      }
      groupings.push_back(all);
    }
    // The keys and GROUPING() calls come first in `projections`, in the order recorded.
    const std::vector<std::string> rest(
        projections.begin() +
            static_cast<std::ptrdiff_t>(key_projections.size() + grouping_projections.size()),
        projections.end());
    std::vector<std::string> branches;
    for (const auto& grouping : groupings) {
      std::vector<std::string> branch;
      std::vector<std::string> partition;
      for (const auto& [id, type] : key_projections) {
        if (grouping.contains(id)) {
          partition.push_back(key_columns.at(id));
          branch.push_back(key_columns.at(id) + " AS " + ColumnName(id));
        } else {
          branch.push_back("CAST(NULL AS " + type + ") AS " + ColumnName(id));
        }
      }
      for (const auto& [id, key] : grouping_projections) {
        branch.push_back(
            std::string(grouping.contains(key) ? "CAST(0 AS BIGINT)" : "CAST(1 AS BIGINT)") +
            " AS " + ColumnName(id));
      }
      branch.insert(branch.end(), rest.begin(), rest.end());
      const std::string window = partition.empty()
                                     ? " OVER () AS "
                                     : " OVER (PARTITION BY " + Join(partition, ", ") + ") AS ";
      std::vector<std::string> over;
      for (const auto& [sql, name] : windows) {
        over.emplace_back(sql).append(window).append(name);
      }
      branches.push_back("SELECT " + Join(branch, ", ") + " FROM (SELECT q.*, " + Join(over, ", ") +
                         input->From() + ") AS q" +
                         (partition.empty() ? "" : " GROUP BY " + Join(partition, ", ")));
    }
    result.sql = Join(branches, " UNION ALL ");
  }
  ApplyFinishes(result, finishes);
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
  std::vector<std::string> intermediates;
  std::map<int, std::string> finishes;
  for (const auto& group : analytic.function_group_list()) {
    for (const auto& computed : group->analytic_function_list()) {
      if (!computed->expr()->Is<googlesql::ResolvedAnalyticFunctionCall>()) {
        return std::nullopt;
      }
      const auto* call = computed->expr()->GetAs<googlesql::ResolvedAnalyticFunctionCall>();
      // GoogleSQL takes PARTITION BY and SAFE with TF_IDF, which BigQuery rejects after analysis.
      if (IsBuiltinFunction(*call->function()) && call->function()->Name() == "tf_idf") {
        if (group->partition_by() != nullptr) {
          throw std::invalid_argument(
              "non-empty OVER() clause is disallowed for function 'tf_idf'");
        }
        if (call->error_mode() == googlesql::ResolvedFunctionCallBase::SAFE_ERROR_MODE) {
          throw std::invalid_argument("SAFE is unsupported with analytic function tf_idf");
        }
      }
      // GoogleSQL takes these with OVER, which BigQuery rejects after analysis.
      static const std::set<std::string_view> kAggregateOnly = {
          "APPROX_COUNT_DISTINCT",
          "HLL_COUNT.INIT",
          "HLL_COUNT.MERGE",
          "HLL_COUNT.MERGE_PARTIAL",
      };
      if (IsBuiltinFunction(*call->function()) &&
          kAggregateOnly.contains(
              ToUpperAscii(call->function()->FullName(/*include_group=*/false)))) {
        throw std::invalid_argument("Analytic function " + ToUpperAscii(call->function()->Name()) +
                                    " is not supported.");
      }
      const auto type = DuckDbType(call->type());
      const auto over = Window(*group, call->window_frame(), scope, input_columns);
      if (!type || !over) {
        return std::nullopt;
      }
      const std::size_t previous = intermediates.size();
      const auto sql =
          NonScalarCall(*call, nullptr, *over, scope, input_columns, "", &intermediates);
      if (!sql) {
        return std::nullopt;
      }
      const int id = computed->column().column_id();
      const std::string cast = "CAST(" + *sql + " AS " + *type + ")";
      if (intermediates.size() == previous) {
        projections.push_back(cast + " AS " + ColumnName(id));
      } else {
        finishes.emplace(id, cast);
      }
      result->columns.emplace(id, "q." + ColumnName(id));
    }
  }
  projections.insert(projections.end(), intermediates.begin(), intermediates.end());
  result->sql = "SELECT " + Join(projections, ", ") + result->From();
  ApplyFinishes(*result, finishes);
  return result;
}

}  // namespace bigquery_emulator_duckdb::translator
