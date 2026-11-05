#include "src/translator/aggregate.h"

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <map>
#include <memory>
#include <numeric>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "googlesql/public/function.h"
#include "googlesql/public/id_string.h"
#include "googlesql/public/type.h"
#include "googlesql/resolved_ast/resolved_ast.h"
#include "googlesql/resolved_ast/resolved_ast_deep_copy_visitor.h"
#include "src/duckdb_sql.h"
#include "src/function_catalog/catalog.h"
#include "src/function_catalog/resolved_access.h"
#include "src/function_catalog/rules.h"
#include "src/language_options.h"
#include "src/translator/collation.h"
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
using function_catalog::IsConstantExpression;
using function_catalog::OrdersOnlyArgument;
using function_catalog::RejectArgumentTypes;
using function_catalog::Translation;
using function_catalog::TypeUse;

namespace {

// Aggregate calls (with `aggregate` set) and analytic calls (with a non-empty `over`). An
// aggregate's HAVING MAX/MIN modifier is the caller's to compute, as the rows `having` selects.
// A rule that orders ties by input names, in `input_row`, the column that numbers the rows.
std::optional<std::string> NonScalarCall(const googlesql::ResolvedNonScalarFunctionCallBase& call,
                                         const googlesql::ResolvedAggregateFunctionCall* aggregate,
                                         const std::string& over, const Scope& scope,
                                         const Columns& columns, const std::string& having = "",
                                         std::vector<std::string>* intermediates = nullptr,
                                         const std::string& partition_order = "",
                                         const std::string& array = "",
                                         const std::string& element = "",
                                         std::string* input_row = nullptr) {
  const auto& functions = scope.context.functions;
  const std::string name = ToUpperAscii(call.function()->FullName(/*include_group=*/false));
  // A graph value's STRUCT is equal to another exactly when their identities are, so functions
  // that only count or compare whole values opt in. BigQuery does not group paths, which
  // GoogleSQL lets DISTINCT do.
  if ((HasGraph(call.type()) ||
       std::ranges::any_of(call.argument_list(),
                           [](const auto& argument) { return HasGraph(argument->type()); })) &&
      (!functions.HasTypeUse(name, TypeUse::kGraphValues) ||
       (call.distinct() && std::ranges::any_of(call.argument_list(), [](const auto& argument) {
          return argument->type()->IsGraphPath();
        })))) {
    return Unsupported(scope, "aggregate or analytic function " + name + " with graph values");
  }
  // BigQuery rejects this prefix on these functions although GoogleSQL accepts it.
  if (functions.HasTypeUse(name, TypeUse::kRejectsSafe) &&
      call.error_mode() == googlesql::ResolvedFunctionCallBase::SAFE_ERROR_MODE) {
    throw std::invalid_argument(
        std::string("SAFE is unsupported with ") +
        (call.Is<googlesql::ResolvedAnalyticFunctionCall>() ? "analytic" : "aggregate") +
        " function " + call.function()->Name());
  }
  if (functions.HasTypeUse(name, TypeUse::kRejectsDistinct) && call.distinct()) {
    throw std::invalid_argument("Aggregate function " + name +
                                " cannot be used with DISTINCT modifier.");
  }
  if (!call.generic_argument_list().empty() || !call.hint_list().empty() ||
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
  // BigQuery rejects HAVING MAX or MIN over collated values, which GoogleSQL accepts.
  if (aggregate != nullptr && aggregate->having_modifier() != nullptr &&
      HasCollation(aggregate->having_modifier()->having_expr()->type_annotation_map())) {
    throw std::invalid_argument("Unsupported having modifier in the aggregate function " +
                                call.function()->Name() + ".");
  }
  const FunctionEntry* entry = functions.Find(name);
  if (entry == nullptr ||
      (entry->translation != Translation::kAggregate &&
       entry->translation != Translation::kAnalytic) ||
      (entry->translation == Translation::kAnalytic && over.empty() && partition_order.empty())) {
    return Unsupported(scope, "aggregate or analytic function " + name);
  }
  const auto type_of = [&](std::size_t argument) {
    return argument < call.argument_list().size()
               ? call.argument_list().at(argument)->type()->kind()
               : googlesql::TYPE_UNKNOWN;
  };
  // Calls that compare values, such as MIN and DISTINCT ones, record the collation they compare
  // under.
  const auto collation = OperationCollation(call.collation_list(), scope);
  if (!collation) {
    return std::nullopt;
  }
  if (!collation->empty() && functions.HasTypeUse(name, TypeUse::kRejectsCollation)) {
    throw std::invalid_argument("Function '" + call.function()->Name() + "' with collation '" +
                                collation->specification + "' is not supported.");
  }
  // Value-returning collated aggregates have STRING-specific backend rules. COUNT can
  // compare recursive keys without changing either its result type or NULL handling.
  if (!collation->children.empty() && name != "COUNT") {
    return Unsupported(scope, "aggregate or analytic function " + name + " with nested collation");
  }
  const auto rule = std::ranges::find_if(entry->aggregates, [&](const AggregateRule& candidate) {
    return candidate.partition_results == !partition_order.empty() &&
           candidate.collated == !collation->empty() &&
           (candidate.type == googlesql::TYPE_UNKNOWN || candidate.type == type_of(0)) &&
           (candidate.distinct != AggregateRule::Distinct::kRequired || call.distinct()) &&
           std::ranges::all_of(candidate.conditions, [&](const Condition& condition) {
             return std::ranges::find(condition.types, type_of(condition.argument - 1)) !=
                    condition.types.end();
           });
  });
  if (rule == entry->aggregates.end() && !collation->empty()) {
    return Unsupported(scope, "aggregate or analytic function " + name + " with collation");
  }
  if (rule != entry->aggregates.end()) {
    for (const auto& requirement : rule->constant_arguments) {
      if (requirement.argument > call.argument_list().size()) {
        continue;
      }
      const auto constant =
          IsConstantExpression(*call.argument_list(static_cast<int>(requirement.argument - 1)));
      if (!constant) {
        return Unsupported(scope, name + " " + std::string(requirement.description));
      }
      if (!*constant) {
        throw std::invalid_argument(std::string(requirement.message));
      }
    }
  }
  RejectArgumentTypes(call, entry->type_rejections);
  if (rule != entry->aggregates.end() && !rule->order_by_argument_error.empty() &&
      aggregate != nullptr && !aggregate->order_by_item_list().empty() &&
      (call.distinct() || OrdersOnlyArgument(*aggregate, 0))) {
    throw std::invalid_argument(std::string(rule->order_by_argument_error));
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
  if (!array.empty()) {
    // List aggregates stay inside the enclosing conditional branch, unlike a correlated
    // scalar subquery, which DuckDB can evaluate even in an unselected branch. Keep the
    // forms to one argument and a rule whose DuckDB call takes one argument; the rule's
    // finish then reads the list aggregate's result as it does the aggregate's. ARRAY_AGG
    // decorates each value with its sort key so sorting and deduplication also stay within
    // list expressions.
    const std::string row = scope.context.FreshName("_value");
    const std::vector<std::string> row_arguments = AggregateArguments(*rule, {row});
    if (aggregate == nullptr || arguments.size() != 1 || !collation->empty() ||
        row_arguments.size() != 1 || rule->distinct == AggregateRule::Distinct::kRequired ||
        rule->distinct == AggregateRule::Distinct::kAlways ||
        (name != "ARRAY_AGG" && (call.distinct() || !aggregate->order_by_item_list().empty())) ||
        aggregate->limit() != nullptr ||
        (name != "SUM" && name != "AVG" && name != "MIN" && name != "MAX" && name != "COUNT" &&
         name != "ARRAY_AGG") ||
        (name != "ARRAY_AGG" && call.null_handling_modifier() ==
                                    googlesql::ResolvedNonScalarFunctionCallBase::RESPECT_NULLS)) {
      return Unsupported(scope, "horizontal aggregate " + name + " with this type or modifiers");
    }
    std::string value = arguments.at(0);
    const bool decorated =
        name == "ARRAY_AGG" && (call.distinct() || !aggregate->order_by_item_list().empty());
    if (decorated) {
      std::vector<std::string> keys;
      for (const auto& item : aggregate->order_by_item_list()) {
        const auto key_collation = OperationCollation(item->collation(), scope);
        if (!key_collation) {
          return std::nullopt;
        }
        if (item->collation_name() != nullptr || !key_collation->empty()) {
          return Unsupported(scope, "horizontal ARRAY_AGG ordered with collation");
        }
        auto key = Expression(*item->column_ref(), scope, columns);
        if (!key) {
          return std::nullopt;
        }
        const auto* key_type = item->column_ref()->type();
        if (key_type->IsInterval()) {
          key = IntervalKey(*key);
        }
        const bool nulls_first =
            item->null_order() == googlesql::ResolvedOrderByItem::NULLS_FIRST ||
            (item->null_order() == googlesql::ResolvedOrderByItem::ORDER_UNSPECIFIED &&
             !item->is_descending());
        const std::string nulls = nulls_first ? " NULLS FIRST" : " NULLS LAST";
        // As in ORDER BY, put NaN below -inf rather than DuckDB's above +inf.
        if (key_type->IsFloatingPoint()) {
          keys.push_back("isnan(" + *key + ")");
          keys.push_back(QuoteLiteral((item->is_descending() ? "ASC" : "DESC") + nulls));
        }
        keys.push_back(*key);
        keys.push_back(QuoteLiteral((item->is_descending() ? "DESC" : "ASC") + nulls));
      }
      value = "struct_pack(" +
              (keys.empty() ? "" : "sort_key := create_sort_key(" + Join(keys, ", ") + "), ") +
              "value := " + value + ")";
    }
    std::string values =
        "list_transform(COALESCE(" + array + ", []), lambda " + element + ": " + value + ")";
    if (rule->skip_nulls ||
        (name == "ARRAY_AGG" && call.null_handling_modifier() ==
                                    googlesql::ResolvedNonScalarFunctionCallBase::IGNORE_NULLS)) {
      values = "list_filter(" + values + ", lambda " + row + ": " + row +
               (decorated ? ".value" : "") + " IS NOT NULL)";
    }
    if (decorated) {
      // Wrapping values in non-NULL structs retains one NULL under DISTINCT, unlike
      // list_distinct on the values themselves. DISTINCT order keys are the argument,
      // as required by the resolver, so deduplicating decorated rows is equivalent.
      if (call.distinct()) {
        values = "list_distinct(" + values + ")";
      }
      if (!aggregate->order_by_item_list().empty()) {
        values = "list_sort(" + values + ", 'ASC', 'NULLS FIRST')";
      }
      values = "list_transform(" + values + ", lambda " + row + ": " + row + ".value)";
    }
    if (!rule->arguments.empty()) {
      values = "list_transform(" + values + ", lambda " + row + ": " + row_arguments.at(0) + ")";
    }
    const std::string sql = "list_aggregate(" + values + ", " + QuoteLiteral(rule->function) + ")";
    const auto type = DuckDbType(call.type(), scope.context.timestamp_precision);
    return type ? std::optional("CAST(" +
                                (rule->finish == nullptr ? sql : rule->finish(sql, arguments, "")) +
                                " AS " + *type + ")")
                : std::nullopt;
  }
  std::vector<std::string> keyed = arguments;
  if (rule->collated) {
    for (std::size_t i = 0; i < arguments.size(); ++i) {
      keyed.push_back(CollationKey(arguments.at(i), *collation,
                                   call.argument_list(static_cast<int>(i))->type()));
    }
  }
  const std::vector<std::string> args = AggregateArguments(*rule, keyed);
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
  } else if (!partition_order.empty()) {
    order = " ORDER BY " + partition_order;
  } else if (const std::string items = AggregateOrder(*rule, arguments); !items.empty()) {
    order = " ORDER BY " + items;
    if (rule->orders_ties_by_input) {
      if (input_row == nullptr) {
        return Unsupported(scope, "aggregate or analytic function " + name + " outside a window");
      }
      if (input_row->empty()) {
        *input_row = scope.context.FreshName("_input_row");
      }
      order += ", q." + *input_row;
    }
  }
  // LIMIT keeps the first elements of the list the aggregate would build; STRING_AGG builds it
  // from its non-NULL values and joins them afterwards.
  std::string limit;
  if (aggregate != nullptr && aggregate->limit() != nullptr) {
    const auto sql = Expression(*aggregate->limit(), scope, columns);
    if (!sql) {
      return std::nullopt;
    }
    // A zero-row aggregate is NULL, not the empty list/string a zero-length slice yields. CASE
    // avoids DuckDB v2's NULLIF macro binder.
    limit = "CASE WHEN " + *sql + " = 0 THEN NULL ELSE " + *sql + " END";
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
  if (rule->collated && rule->distinct == AggregateRule::Distinct::kRequired) {
    // DuckDB's ORDER BY need not keep equal values in input order, so the values are collected as
    // they come, and the first ORDER BY item, over the argument, sorts the distinct ones.
    int direction = 0;
    bool nulls_first = false;
    if (aggregate != nullptr && !aggregate->order_by_item_list().empty()) {
      const auto& item = *aggregate->order_by_item_list(0);
      const auto* argument = call.argument_list(0)->GetAs<googlesql::ResolvedColumnRef>();
      if (!call.argument_list(0)->Is<googlesql::ResolvedColumnRef>() ||
          argument->column() != item.column_ref()->column()) {
        return Unsupported(scope, name + "(DISTINCT ...) ordered by another expression");
      }
      direction = item.is_descending() ? -1 : 1;
      nulls_first = item.null_order() == googlesql::ResolvedOrderByItem::NULLS_FIRST ||
                    (item.null_order() == googlesql::ResolvedOrderByItem::ORDER_UNSPECIFIED &&
                     !item.is_descending());
    }
    std::string sql = "bq_collation_distinct(" + std::string(rule->function) + "(" + args.at(0) +
                      ")" + tail + ", " + QuoteLiteral(collation->specification) + ", " +
                      std::to_string(direction) + ", " + (nulls_first ? "true" : "false") + ")";
    if (!limit.empty()) {
      sql = "list_slice(" + sql + ", 1, " + limit + ")";
    }
    return finish(sql, tail);
  }
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

std::optional<std::vector<std::string>> PartitionKeys(
    const googlesql::ResolvedAnalyticFunctionGroup& group, const Scope& scope,
    const Columns& columns) {
  std::vector<std::string> keys;
  if (const auto* partition = group.partition_by(); partition != nullptr) {
    if (!partition->hint_list().empty()) {
      return std::nullopt;
    }
    for (int i = 0; i < partition->partition_by_list_size(); ++i) {
      const auto& ref = partition->partition_by_list(i);
      const auto collation = partition->collation_list().empty()
                                 ? std::optional<Collation>(Collation{})
                                 : OperationCollation(partition->collation_list(i), scope);
      if (!collation) {
        return std::nullopt;
      }
      if (HasGraph(ref->type())) {
        Unsupported(scope, "PARTITION BY with graph values");
        return std::nullopt;
      }
      if (scope.context.functions.restrictions().scalar_window_partition_keys_only &&
          (ref->type()->IsStruct() || ref->type()->IsArray())) {
        throw std::invalid_argument(std::string("Partitioning by expressions of type ") +
                                    (ref->type()->IsStruct() ? "STRUCT" : "ARRAY") +
                                    " is unsupported");
      }
      if (HasInterval(ref->type()) && !ref->type()->IsInterval()) {
        Unsupported(scope, "PARTITION BY with INTERVAL");
        return std::nullopt;
      }
      const auto key = Expression(*ref, scope, columns);
      if (!key) {
        return std::nullopt;
      }
      keys.push_back(CollationKey(ref->type()->IsInterval() ? IntervalKey(*key) : *key, *collation,
                                  ref->type()));
    }
  }
  return keys;
}

std::optional<std::string> Window(const googlesql::ResolvedAnalyticFunctionGroup& group,
                                  const googlesql::ResolvedWindowFrame* frame, const Scope& scope,
                                  const Columns& columns) {
  std::vector<std::string> window;
  const auto keys = PartitionKeys(group, scope, columns);
  if (!keys) {
    return std::nullopt;
  }
  if (!keys->empty()) {
    window.push_back("PARTITION BY " + Join(*keys, ", "));
  }
  if (const auto* ordering = group.order_by(); ordering != nullptr) {
    if (!ordering->hint_list().empty()) {
      return std::nullopt;
    }
    const auto is_offset = [](const googlesql::ResolvedWindowFrameExpr* bound) {
      return bound->boundary_type() == googlesql::ResolvedWindowFrameExpr::OFFSET_PRECEDING ||
             bound->boundary_type() == googlesql::ResolvedWindowFrameExpr::OFFSET_FOLLOWING;
    };
    const auto order = OrderItems(
        ordering->order_by_item_list(), scope, columns,
        frame != nullptr && frame->frame_unit() == googlesql::ResolvedWindowFrame::RANGE &&
            (is_offset(frame->start_expr()) || is_offset(frame->end_expr())));
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

// What a GROUP BY key groups by: INTERVAL values by their lengths, graph elements by identity
// and strings by their keys under the key's `collation`.
std::string GroupKey(const googlesql::Type* type, const std::string& sql,
                     const Collation& collation) {
  return type->IsGraphElement() ? "struct_extract(" + sql + ", '$id')"
                                : CollationKey(IntervalKey(type, sql), collation, type);
}

}  // namespace

std::optional<std::string> ArrayAggregate(const googlesql::ResolvedArrayAggregate& aggregate,
                                          const Scope& scope, const Columns& columns) {
  auto array = Expression(*aggregate.array(), scope, columns);
  if (!array) {
    return std::nullopt;
  }
  // Each lambda row carries the element and computed columns. Materialize each computed
  // expression once per element, preserving traversal order and references to enclosing rows.
  const std::string element = scope.context.FreshName("_horizontal");
  const std::string element_name = ColumnName(aggregate.element_column().column_id());
  *array = "list_transform(" + *array + ", lambda " + element + ": struct_pack(" + element_name +
           " := " + element + "))";
  Columns bound = columns;
  bound[aggregate.element_column().column_id()] = element + "." + element_name;
  std::vector<std::string> fields = {element_name + " := " + element + "." + element_name};
  for (const auto& computed : aggregate.pre_aggregate_computed_column_list()) {
    const auto value = Expression(*computed->expr(), scope, bound);
    if (!value) {
      return std::nullopt;
    }
    const std::string name = ColumnName(computed->column().column_id());
    auto projection = fields;
    std::string field = name;
    field += " := ";
    field += *value;
    projection.push_back(std::move(field));
    array->insert(0, "list_transform(");
    *array += ", lambda ";
    *array += element;
    *array += ": struct_pack(";
    *array += Join(projection, ", ");
    *array += "))";
    std::string reference = element;
    reference += '.';
    reference += name;
    std::string binding = name;
    binding += " := ";
    binding += reference;
    fields.push_back(std::move(binding));
    bound[computed->column().column_id()] = std::move(reference);
  }
  return NonScalarCall(*aggregate.aggregate(), aggregate.aggregate(), "", scope, bound, "", nullptr,
                       "", *array, element);
}

namespace {

std::optional<Relation> AggregateRelation(const googlesql::ResolvedAggregateScan& aggregate,
                                          const Scope& scope, std::optional<Relation> input,
                                          std::vector<Collation> collations = {}) {
  // Keys with INTERVAL values group by their lengths, and collated strings by their collation keys,
  // and return the first of equal values in input order, as BigQuery does. Graph elements group by
  // identity, as SAME compares them.
  const bool grouping_sets =
      !aggregate.grouping_set_list().empty() || !aggregate.grouping_call_list().empty();
  if (std::ranges::any_of(aggregate.group_by_list(), [&](const auto& key) {
        const googlesql::Type* type = key->expr()->type();
        return HasGraph(type) && (grouping_sets || !type->IsGraphElement());
      })) {
    return Unsupported(scope, "GROUP BY with graph values");
  }
  // The collation of each key, which SELECT DISTINCT records as GROUP BY does.
  for (int i = static_cast<int>(collations.size()); i < aggregate.group_by_list_size(); ++i) {
    const auto collation = aggregate.collation_list().empty()
                               ? std::optional<Collation>(Collation{})
                               : OperationCollation(aggregate.collation_list(i), scope);
    if (!collation) {
      return std::nullopt;
    }
    collations.push_back(*collation);
  }
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
  // The aggregate that returns each collated key's value in a group of its key's grouping.
  std::map<int, std::string> key_values;
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
    // BigQuery returns the first of equal INTERVAL values in the whole input, also in the groups
    // of later rows, so a key with INTERVAL values becomes that value before the grouping. Values
    // with different lengths differ in DuckDB too, so DuckDB groups them as BigQuery does. A
    // collated key returns the first of its equal values in each group instead, so it groups by
    // its collation key and returns the value as an aggregate.
    std::vector<std::string> firsts;
    std::vector<std::string> collation_keys;
    for (int i = 0; i < aggregate.group_by_list_size(); ++i) {
      const auto& key = aggregate.group_by_list(i);
      const auto sql = Expression(*key->expr(), scope, input->columns);
      if (!sql) {
        return std::nullopt;
      }
      const int id = key->column().column_id();
      inner.push_back(*sql + " AS " + ColumnName(id));
      const std::string column = "q." + ColumnName(id);
      if (HasInterval(key->expr()->type())) {
        firsts.push_back("first_value(" + column + ") OVER (PARTITION BY " +
                         IntervalKey(key->expr()->type(), column) + " ORDER BY q._rn) AS " +
                         ColumnName(id));
      }
      const auto type = DuckDbType(key->expr()->type(), scope.context.timestamp_precision);
      if (!type) {
        return std::nullopt;
      }
      key_projections.emplace_back(id, *type);
      if (collations.at(i).empty()) {
        key_columns.emplace(id, column);
        projections.push_back(column + " AS " + ColumnName(id));
      } else {
        const std::string name = QuoteIdentifier("_k" + std::to_string(id));
        collation_keys.push_back(CollationKey(column, collations.at(i), key->expr()->type()) +
                                 " AS " + name);
        key_columns.emplace(id, "q." + name);
        const std::string value = "first(" + column + ")";
        key_values.emplace(id, value);
        projections.push_back(std::string("CASE WHEN GROUPING(q.")
                                  .append(name)
                                  .append(") = 0 THEN ")
                                  .append(value)
                                  .append(" END AS ")
                                  .append(ColumnName(id)));
      }
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
    if (!collation_keys.empty()) {
      sql = "SELECT q.*, " + Join(collation_keys, ", ") + " FROM (" + sql + ") AS q";
    }
    input = Relation{.sql = sql, .columns = columns};
  } else {
    // HAVING MAX/MIN keeps the rows whose expression reaches the group's maximum or minimum,
    // which a window over the group computes alongside the input; INTERVAL values compare by
    // their keys.
    std::vector<std::string> partition;
    std::vector<std::string> partition_values;
    for (int i = 0; i < aggregate.group_by_list_size(); ++i) {
      const auto& key = aggregate.group_by_list(i);
      const auto sql = Expression(*key->expr(), scope, input->columns);
      if (!sql) {
        return std::nullopt;
      }
      const std::string name = "_partition" + std::to_string(partition.size());
      partition_values.push_back(GroupKey(key->expr()->type(), *sql, collations.at(i)) + " AS " +
                                 name);
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
      const std::string over =
          " OVER (" + (partition.empty() ? "" : "PARTITION BY " + Join(partition, ", ")) + ")";
      inner.push_back(std::string(modifier.kind() == googlesql::ResolvedAggregateHavingModifier::MAX
                                      ? "max("
                                      : "min(")
                          .append(IntervalKey(type, *window))
                          .append(")")
                          .append(over)
                          .append(" AS ")
                          .append(name));
      std::string having = IntervalKey(type, *row) + " = q." + name;
      if (type->IsFloatingPoint()) {
        // BigQuery's MIN/MAX propagates NaN, and SQL equality never matches NaN, so a
        // group containing any NaN keeps no rows. DuckDB's MIN ignores NaN beside finite
        // values, and its equality matches NaN, so guard both modifiers explicitly.
        inner.push_back(std::string("bool_or(isnan(")
                            .append(*window)
                            .append("))")
                            .append(over)
                            .append(" AS ")
                            .append(name)
                            .append("_nan"));
        having += " AND NOT q." + name + "_nan";
      }
      havings.emplace(call, std::move(having));
    }
    if (!havings.empty()) {
      // A guarded expression can contain error(), which DuckDB v2 considers volatile and
      // forbids directly in a window's PARTITION BY. Bind the keys below the window instead.
      if (!partition_values.empty()) {
        input->sql = "SELECT q.*, " + Join(partition_values, ", ") + input->From();
      }
      input = Relation{.sql = "SELECT " + Join(inner, ", ") + input->From(), .columns = columns};
    }
    for (int i = 0; i < aggregate.group_by_list_size(); ++i) {
      const auto& key = aggregate.group_by_list(i);
      const auto sql = Expression(*key->expr(), scope, input->columns);
      if (!sql) {
        return std::nullopt;
      }
      const int id = key->column().column_id();
      if (const googlesql::Type* type = key->expr()->type();
          HasInterval(type) || type->IsGraphElement() || !collations.at(i).empty()) {
        projections.push_back("first(" + *sql + ") AS " + ColumnName(id));
        keys.push_back(GroupKey(type, *sql, collations.at(i)));
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
      std::string having = *sql + " = q." + name;
      if (modifier.having_expr()->type()->IsFloatingPoint()) {
        // As without grouping sets, a NaN anywhere in this group prevents every match.
        windows.emplace_back("bool_or(isnan(" + *sql + "))", name + "_nan");
        having += " AND NOT q." + name + "_nan";
      }
      havings.emplace(call, std::move(having));
    }
  }
  for (const auto& computed : aggregate.aggregate_list()) {
    const auto* call = computed->expr()->GetAs<googlesql::ResolvedAggregateFunctionCall>();
    const auto type = DuckDbType(computed->expr()->type(), scope.context.timestamp_precision);
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
          const auto value = key_values.find(id);
          branch.push_back((value == key_values.end() ? key_columns.at(id) : value->second) +
                           " AS " + ColumnName(id));
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

}  // namespace

std::optional<Relation> AggregateScan(const googlesql::ResolvedAggregateScan& aggregate,
                                      const Scope& scope) {
  return AggregateRelation(aggregate, scope, Scan(*aggregate.input_scan(), scope));
}

std::optional<Relation> PivotScan(const googlesql::ResolvedPivotScan& pivot, const Scope& scope) {
  // PIVOT records no comparison collation list; its FOR expression's annotation
  // supplies the collation for comparisons with every IN value.
  const auto pivot_collation = ColumnCollation(pivot.for_expr()->type_annotation_map());
  const auto input = Scan(*pivot.input_scan(), scope);
  if (!input) {
    return std::nullopt;
  }
  // Bind the pivot key, grouping keys and aggregate inputs once, before dividing the rows
  // among pivot values. In particular, RAND() must not be evaluated anew for each value.
  const std::string source = scope.context.FreshName("_pivot_input");
  const std::string groups = scope.context.FreshName("_pivot_groups");
  std::vector<std::string> bound;
  Columns columns;
  for (const auto& [id, sql] : input->columns) {
    bound.push_back(sql + " AS " + ColumnName(id));
    columns.emplace(id, "q." + ColumnName(id));
  }
  // Allocate above every input, output and correlated column in this subtree. The local
  // bindings do not escape the PIVOT relation, but must not shadow its subqueries' columns.
  const auto max_id = [](int id, const googlesql::ResolvedColumn& column) {
    return std::max(id, column.column_id());
  };
  int next_column_id =
      std::accumulate(pivot.column_list().begin(), pivot.column_list().end(), 0, max_id);
  // Grouping keys still affect row counts when their output columns have been pruned.
  next_column_id =
      std::accumulate(pivot.group_by_list().begin(), pivot.group_by_list().end(), next_column_id,
                      [&](int id, const auto& key) { return max_id(id, key->column()); });
  std::vector<const googlesql::ResolvedNode*> nodes;
  pivot.GetDescendantsSatisfying(&googlesql::ResolvedNode::Is<googlesql::ResolvedScan>, &nodes);
  for (const auto* node : nodes) {
    const auto& scan_columns = node->GetAs<googlesql::ResolvedScan>()->column_list();
    next_column_id =
        std::accumulate(scan_columns.begin(), scan_columns.end(), next_column_id, max_id);
  }
  nodes.clear();
  pivot.GetDescendantsWithKinds({googlesql::RESOLVED_COLUMN_REF}, &nodes);
  next_column_id =
      std::accumulate(nodes.begin(), nodes.end(), next_column_id, [&](int id, const auto* node) {
        return max_id(id, node->template GetAs<googlesql::ResolvedColumnRef>()->column());
      });
  googlesql::IdStringPool names;
  const auto name = names.Make("_pivot");
  const auto bind =
      [&](const googlesql::ResolvedExpr& expr) -> std::unique_ptr<googlesql::ResolvedColumnRef> {
    const auto sql = Expression(expr, scope, input->columns);
    if (!sql) {
      return nullptr;
    }
    const googlesql::ResolvedColumn column(++next_column_id, name, name, expr.annotated_type());
    const std::string alias = ColumnName(column.column_id());
    bound.push_back(*sql + " AS " + alias);
    columns.emplace(column.column_id(), "q." + alias);
    return googlesql::MakeResolvedColumnRef(expr.type(), column, false);
  };
  auto pivot_key = bind(*pivot.for_expr());
  if (!pivot_key) {
    return std::nullopt;
  }
  const std::string pivot_sql = columns.at(pivot_key->column().column_id());
  auto grouping = googlesql::MakeResolvedAggregateScan();
  std::vector<std::string> group_conditions;
  std::vector<Collation> group_collations;
  for (const auto& key : pivot.group_by_list()) {
    auto ref = bind(*key->expr());
    if (!ref) {
      return std::nullopt;
    }
    const std::string sql = columns.at(ref->column().column_id());
    // PIVOT records no grouping collation list, so each implicit key uses its column's
    // annotation, as a DISTINCT set operation does.
    const auto collation = ColumnCollation(key->column().type_annotation_map());
    group_collations.push_back(collation);
    group_conditions.push_back(
        GroupKey(key->expr()->type(), sql, collation) + " IS NOT DISTINCT FROM " +
        GroupKey(key->expr()->type(), "g." + ColumnName(key->column().column_id()), collation));
    grouping->add_group_by_list(
        googlesql::MakeResolvedComputedColumn(key->column(), std::move(ref)));
  }
  std::vector<std::unique_ptr<googlesql::ResolvedAggregateFunctionCall>> calls;
  for (const auto& expr : pivot.pivot_expr_list()) {
    if (!expr->Is<googlesql::ResolvedAggregateFunctionCall>()) {
      return Unsupported(scope, "PIVOT expression other than an aggregate call");
    }
    auto copy = googlesql::ResolvedASTDeepCopyVisitor::Copy(
        expr->GetAs<googlesql::ResolvedAggregateFunctionCall>());
    if (!copy.ok()) {
      throw std::invalid_argument(std::string(copy.status().message()));
    }
    auto call = std::move(*copy);
    auto arguments = call->release_argument_list();
    for (auto& argument : arguments) {
      const auto constant = IsConstantExpression(*argument);
      if (!constant) {
        return Unsupported(scope, "PIVOT aggregate argument");
      }
      // Keep constants (such as STRING_AGG delimiters) and already-bound column references.
      // The latter also preserve DISTINCT aggregates' ORDER BY argument identity.
      if (*constant || argument->Is<googlesql::ResolvedColumnRef>()) {
        continue;
      }
      auto ref = bind(*argument);
      if (!ref) {
        return std::nullopt;
      }
      argument = std::move(ref);
    }
    call->set_argument_list(std::move(arguments));
    if (call->having_modifier() != nullptr) {
      auto modifier = googlesql::ResolvedASTDeepCopyVisitor::Copy(call->having_modifier());
      if (!modifier.ok()) {
        throw std::invalid_argument(std::string(modifier.status().message()));
      }
      auto ref = bind(*(*modifier)->having_expr());
      if (!ref) {
        return std::nullopt;
      }
      (*modifier)->set_having_expr(std::move(ref));
      call->set_having_modifier(std::move(*modifier));
    }
    calls.push_back(std::move(call));
  }
  const Relation source_rows{.sql = "SELECT * FROM " + source, .columns = columns};
  std::optional<Relation> grouped;
  if (pivot.group_by_list().empty()) {
    // A global PIVOT has one row even for empty input, just like a global aggregate.
    grouped = Relation{.sql = "SELECT 1 AS _unit", .columns = {}};
  } else {
    grouped = AggregateRelation(*grouping, scope, source_rows, std::move(group_collations));
  }
  if (!grouped) {
    return std::nullopt;
  }
  std::string from = " FROM " + groups + " AS g";
  std::vector<std::string> projections;
  Relation result;
  for (const auto& key : pivot.group_by_list()) {
    const int id = key->column().column_id();
    projections.push_back("g." + ColumnName(id) + " AS " + ColumnName(id));
    result.columns.emplace(id, "q." + ColumnName(id));
  }
  // A lateral global aggregate also handles missing pivot values: COUNT returns zero,
  // while ARRAY_AGG and the other aggregates return their normal empty-input results.
  for (const auto& output : pivot.pivot_column_list()) {
    const int id = output->column().column_id();
    const auto value =
        Expression(*pivot.pivot_value_list(output->pivot_value_index()), scope, input->columns);
    if (!value) {
      return std::nullopt;
    }
    std::vector<std::string> conditions = group_conditions;
    conditions.push_back(GroupKey(pivot.for_expr()->type(), pivot_sql, pivot_collation) +
                         " IS NOT DISTINCT FROM " +
                         GroupKey(pivot.for_expr()->type(), *value, pivot_collation));
    Relation rows{
        .sql = "SELECT q.*" + source_rows.From() + " WHERE " + Join(conditions, " AND "),
        .columns = columns,
    };
    auto aggregate = googlesql::MakeResolvedAggregateScan();
    auto call =
        googlesql::ResolvedASTDeepCopyVisitor::Copy(calls.at(output->pivot_expr_index()).get());
    if (!call.ok()) {
      throw std::invalid_argument(std::string(call.status().message()));
    }
    aggregate->add_aggregate_list(
        googlesql::MakeResolvedComputedColumn(output->column(), std::move(*call)));
    const auto relation = AggregateRelation(*aggregate, scope, std::move(rows));
    if (!relation) {
      return std::nullopt;
    }
    const std::string alias = scope.context.FreshName("_pivot_value");
    from += " CROSS JOIN LATERAL (" + relation->sql + ") AS " + alias;
    projections.push_back(alias + "." + ColumnName(id) + " AS " + ColumnName(id));
    result.columns.emplace(id, "q." + ColumnName(id));
  }
  result.sql = "WITH " + source + " AS MATERIALIZED (SELECT " + Join(bound, ", ") + input->From() +
               "), " + groups + " AS MATERIALIZED (" + grouped->sql + ") SELECT " +
               (projections.empty() ? "1 AS _unit" : Join(projections, ", ")) + from;
  return result;
}

namespace {

bool HasPartitionResults(const googlesql::ResolvedAnalyticFunctionCall& call, const Scope& scope) {
  const auto* entry = scope.context.functions.Find(
      ToUpperAscii(call.function()->FullName(/*include_group=*/false)));
  return IsBuiltinFunction(*call.function()) && entry != nullptr &&
         entry->translation == Translation::kAnalytic &&
         std::ranges::any_of(entry->aggregates,
                             [](const AggregateRule& rule) { return rule.partition_results; });
}

// Materialize the input identity and the grouped result once. Computing an array-valued finish
// over list(...) OVER (...) on every row would repeat the whole partition's computation.
bool PartitionResults(Relation& input, const googlesql::ResolvedAnalyticFunctionGroup& group,
                      const googlesql::ResolvedAnalyticFunctionCall& call, int output_id,
                      const Scope& scope) {
  if (call.window_frame() != nullptr) {
    Unsupported(scope, "partition-result analytic function with a window frame");
    return false;
  }
  const auto keys = PartitionKeys(group, scope, input.columns);
  if (!keys) {
    return false;
  }
  const std::string row = scope.context.FreshName("_partition_row");
  std::string order;
  if (const auto* ordering = group.order_by(); ordering != nullptr) {
    if (!ordering->hint_list().empty()) {
      return false;
    }
    const auto items = OrderItems(ordering->order_by_item_list(), scope, input.columns);
    if (!items) {
      return false;
    }
    order = *items + ", ";
  }
  order += "q." + row;
  const auto values = NonScalarCall(call, nullptr, "", scope, input.columns, "", nullptr, order);
  if (!values) {
    return false;
  }
  const std::string source = scope.context.FreshName("_partition_input");
  const std::string grouped = scope.context.FreshName("_partition_groups");
  const auto type = DuckDbType(call.type(), scope.context.timestamp_precision);
  if (!type) {
    return false;
  }
  input.sql = "WITH " + source + " AS MATERIALIZED (SELECT q.*, row_number() OVER () AS " + row +
              input.From() + "), " + grouped + " AS MATERIALIZED (SELECT list(q." + row +
              " ORDER BY " + order + ") AS ids, " + *values + " AS vals FROM " + source + " AS q" +
              (keys->empty() ? "" : " GROUP BY " + Join(*keys, ", ")) +
              ") SELECT q.*, CAST(p.val AS " + *type + ") AS " + ColumnName(output_id) + " FROM " +
              source + " AS q JOIN (SELECT unnest(ids) AS id, unnest(vals) AS val FROM " + grouped +
              ") AS p ON q." + row + " = p.id";
  input.columns.emplace(output_id, "q." + ColumnName(output_id));
  return true;
}

}  // namespace

std::optional<Relation> AnalyticScan(const googlesql::ResolvedAnalyticScan& analytic,
                                     const Scope& scope) {
  auto result = Scan(*analytic.input_scan(), scope);
  if (!result) {
    return std::nullopt;
  }
  for (const auto& group : analytic.function_group_list()) {
    for (const auto& computed : group->analytic_function_list()) {
      if (!computed->expr()->Is<googlesql::ResolvedAnalyticFunctionCall>()) {
        return std::nullopt;
      }
      const auto* call = computed->expr()->GetAs<googlesql::ResolvedAnalyticFunctionCall>();
      if (HasPartitionResults(*call, scope) &&
          !PartitionResults(*result, *group, *call, computed->column().column_id(), scope)) {
        return std::nullopt;
      }
    }
  }
  const Columns input_columns = result->columns;
  std::vector<std::string> projections = {"q.*"};
  std::vector<std::string> intermediates;
  std::map<int, std::string> finishes;
  std::string input_row;
  for (const auto& group : analytic.function_group_list()) {
    for (const auto& computed : group->analytic_function_list()) {
      if (!computed->expr()->Is<googlesql::ResolvedAnalyticFunctionCall>()) {
        return std::nullopt;
      }
      const auto* call = computed->expr()->GetAs<googlesql::ResolvedAnalyticFunctionCall>();
      if (HasPartitionResults(*call, scope)) {
        continue;
      }
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
      if (IsBuiltinFunction(*call->function()) &&
          scope.context.functions.HasTypeUse(
              ToUpperAscii(call->function()->FullName(/*include_group=*/false)),
              TypeUse::kRejectsOver)) {
        throw std::invalid_argument("Analytic function " + ToUpperAscii(call->function()->Name()) +
                                    " is not supported.");
      }
      if (IsBuiltinFunction(*call->function()) && call->distinct() &&
          scope.context.functions.HasTypeUse(
              ToUpperAscii(call->function()->FullName(/*include_group=*/false)),
              TypeUse::kRejectsAnalyticDistinct)) {
        throw std::invalid_argument("Analytic function " + call->function()->Name() +
                                    " does not support DISTINCT.");
      }
      const auto type = DuckDbType(call->type(), scope.context.timestamp_precision);
      const auto over = Window(*group, call->window_frame(), scope, input_columns);
      if (!type || !over) {
        return std::nullopt;
      }
      const std::size_t previous = intermediates.size();
      const auto sql = NonScalarCall(*call, nullptr, *over, scope, input_columns, "",
                                     &intermediates, "", "", "", &input_row);
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
  if (!input_row.empty()) {
    result->sql = "SELECT q.*, row_number() OVER () AS " + input_row + result->From();
  }
  result->sql = "SELECT " + Join(projections, ", ") + result->From();
  ApplyFinishes(*result, finishes);
  return result;
}

}  // namespace bigquery_emulator_duckdb::translator
