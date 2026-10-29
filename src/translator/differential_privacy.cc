#include "src/translator/differential_privacy.h"

#include <cmath>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/ascii.h"
#include "googlesql/public/anonymization_utils.h"
#include "googlesql/public/constant_evaluator.h"
#include "googlesql/public/type.h"
#include "googlesql/public/value.h"
#include "googlesql/resolved_ast/resolved_ast.h"
#include "src/duckdb_sql.h"
#include "src/translator/context.h"
#include "src/translator/expression.h"
#include "src/translator/literal.h"
#include "src/translator/scan.h"
#include "src/type_mapping.h"

namespace bigquery_emulator_duckdb::translator {
namespace {

// Privacy options and contribution bounds, which BigQuery requires to be literals.
class LiteralEvaluator : public googlesql::ConstantEvaluator {
 public:
  absl::StatusOr<googlesql::Value> Evaluate(const googlesql::ResolvedExpr& expression) override {
    if (!expression.Is<googlesql::ResolvedLiteral>()) {
      return absl::UnimplementedError("differential privacy options other than literals");
    }
    return expression.GetAs<googlesql::ResolvedLiteral>()->value();
  }
};

// The DuckDB function computing the differentially private aggregate `call` over the list of
// its privacy units' contributions, for the functions and types BigQuery documents.
std::optional<std::string_view> Function(const googlesql::ResolvedAggregateFunctionCall& call) {
  const std::string name = call.function()->Name();
  const googlesql::TypeKind type = call.argument_list(0)->type()->kind();
  // BigQuery's answers for the aggregates it does not document.
  if (name == "$differential_privacy_var_pop" || name == "$differential_privacy_stddev_pop") {
    throw std::invalid_argument("Function not found: `" + name + "`");
  }
  if ((name == "$differential_privacy_sum" || name == "$differential_privacy_avg") &&
      type == googlesql::TYPE_NUMERIC) {
    throw std::invalid_argument(name + " aggregate function is not supported.");
  }
  if (call.type()->kind() != type || call.argument_list_size() > 2) {
    return std::nullopt;
  }
  if (name == "$differential_privacy_sum" && type == googlesql::TYPE_INT64) {
    return "bq_dp_sum_int64";
  }
  if (name == "$differential_privacy_sum" && type == googlesql::TYPE_DOUBLE) {
    return "bq_dp_sum_float64";
  }
  if (name == "$differential_privacy_avg" && type == googlesql::TYPE_DOUBLE) {
    return "bq_dp_avg";
  }
  return std::nullopt;
}

// The lower and upper bounds of the call's contribution_bounds_per_group, NULL for none.
std::optional<std::pair<std::string, std::string>> Bounds(
    const googlesql::ResolvedAggregateFunctionCall& call, const Scope& scope) {
  if (call.argument_list_size() < 2) {
    return std::pair<std::string, std::string>("NULL", "NULL");
  }
  const googlesql::ResolvedExpr& bounds = *call.argument_list(1);
  if (!bounds.Is<googlesql::ResolvedLiteral>()) {
    return Unsupported(scope, "contribution bounds other than literals");
  }
  const googlesql::Value& value = bounds.GetAs<googlesql::ResolvedLiteral>()->value();
  if (value.is_null()) {
    return std::pair<std::string, std::string>("NULL", "NULL");
  }
  if (value.field(0).is_null() || value.field(1).is_null()) {
    return Unsupported(scope, "contribution bounds with NULL");
  }
  const auto lower = Literal(value.field(0), scope.context.timestamp_precision);
  const auto upper = Literal(value.field(1), scope.context.timestamp_precision);
  if (!lower || !upper) {
    return std::nullopt;
  }
  return std::pair(*lower, *upper);
}

// The rows the rewriter samples so that each privacy unit contributes to at most
// max_groups_contributed groups: a random selection of each unit's rows, which are its groups.
std::optional<Relation> Contributions(const googlesql::ResolvedScan& input, const Scope& scope) {
  if (!input.Is<googlesql::ResolvedSampleScan>()) {
    return Scan(input, scope);
  }
  const auto& sample = *input.GetAs<googlesql::ResolvedSampleScan>();
  if (absl::AsciiStrToLower(sample.method()) != "reservoir" ||
      sample.unit() != googlesql::ResolvedSampleScan::ROWS ||
      !sample.size()->Is<googlesql::ResolvedLiteral>() || sample.repeatable_argument() != nullptr ||
      sample.weight_column() != nullptr || sample.partition_by_list().empty()) {
    return Unsupported(scope, "differential privacy input sampled otherwise than per unit");
  }
  auto result = Scan(*sample.input_scan(), scope);
  if (!result) {
    return std::nullopt;
  }
  std::vector<std::string> partition;
  for (const auto& expression : sample.partition_by_list()) {
    const auto sql = Expression(*expression, scope, result->columns);
    if (!sql) {
      return std::nullopt;
    }
    partition.push_back(*sql);
  }
  const auto size = Literal(sample.size()->GetAs<googlesql::ResolvedLiteral>()->value(),
                            scope.context.timestamp_precision);
  if (!size) {
    return std::nullopt;
  }
  // DuckDB orders windows only by non-volatile expressions, so the random order is a column.
  result->sql = "SELECT q.* EXCLUDE (_sample) FROM (SELECT q.*, random() AS _sample" +
                result->From() + ") AS q QUALIFY row_number() OVER (PARTITION BY " +
                Join(partition, ", ") + " ORDER BY q._sample) <= " + *size;
  return result;
}

void Check(const absl::Status& status) {
  if (!status.ok()) {
    throw std::invalid_argument(std::string(status.message()));
  }
}

}  // namespace

std::optional<Relation> DifferentialPrivacyAggregateScan(
    const googlesql::ResolvedDifferentialPrivacyAggregateScan& aggregate, const Scope& scope) {
  if (!aggregate.grouping_set_list().empty() || !aggregate.grouping_call_list().empty() ||
      !aggregate.collation_list().empty()) {
    return Unsupported(scope, "differential privacy with grouping sets or collations");
  }
  // The options BigQuery documents; the analyzer has taken privacy_unit_column.
  std::optional<googlesql::Value> delta;
  std::optional<googlesql::Value> max_groups_contributed;
  for (const auto& option : aggregate.option_list()) {
    const std::string name = absl::AsciiStrToLower(option->name());
    if (name != "epsilon" && name != "delta" && name != "max_groups_contributed") {
      return Unsupported(scope, "differential privacy option " + name);
    }
    if (!option->value()->Is<googlesql::ResolvedLiteral>()) {
      return Unsupported(scope, "differential privacy options other than literals");
    }
    const googlesql::Value& value = option->value()->GetAs<googlesql::ResolvedLiteral>()->value();
    if (name == "delta") {
      delta = value;
    } else if (name == "max_groups_contributed") {
      max_groups_contributed = value;
    }
  }
  // The reference implementation's checks of the options the analyzer leaves to engines.
  absl::StatusOr<std::unique_ptr<googlesql::anonymization::FunctionEpsilonAssigner>> assigner =
      googlesql::anonymization::FunctionEpsilonAssigner::CreateFromScan(
          &aggregate, std::make_unique<LiteralEvaluator>());
  if (!assigner.ok() && assigner.status().code() == absl::StatusCode::kUnimplemented) {
    return Unsupported(scope, assigner.status().message());
  }
  Check(assigner.status());
  if (!delta) {
    throw std::invalid_argument("Differential privacy option DELTA must be set and non-NULL.");
  }
  if (delta->is_null()) {
    throw std::invalid_argument("Differential privacy option DELTA must be set and non-NULL.");
  }
  if (delta->double_value() <= 0 || delta->double_value() > 1 ||
      !std::isfinite(delta->double_value())) {
    throw std::invalid_argument("Delta must be finite, positive, and no greater than 1 but is " +
                                delta->DebugString());
  }
  const bool bounded = max_groups_contributed && !max_groups_contributed->is_null();
  const std::string max_groups_sql =
      bounded ? std::to_string(max_groups_contributed->int64_value()) : "NULL";

  auto input = Contributions(*aggregate.input_scan(), scope);
  if (!input) {
    return std::nullopt;
  }
  std::vector<std::string> projections;
  std::vector<std::string> keys;
  for (const auto& key : aggregate.group_by_list()) {
    const googlesql::Type* type = key->expr()->type();
    if (HasInterval(type) || HasGraph(type) || HasRange(type)) {
      return Unsupported(scope, "differential privacy GROUP BY " + type->DebugString());
    }
    const auto sql = Expression(*key->expr(), scope, input->columns);
    if (!sql) {
      return std::nullopt;
    }
    const int id = key->column().column_id();
    projections.push_back(*sql + " AS " + ColumnName(id));
    keys.push_back(std::to_string(projections.size()));
  }
  const googlesql::ResolvedExpr* threshold_expression = aggregate.group_selection_threshold_expr();
  if (threshold_expression == nullptr ||
      !threshold_expression->Is<googlesql::ResolvedColumnRef>()) {
    return Unsupported(scope, "differential privacy group selection");
  }
  const int threshold_column =
      threshold_expression->GetAs<googlesql::ResolvedColumnRef>()->column().column_id();
  std::optional<double> threshold_epsilon;
  for (const auto& computed : aggregate.aggregate_list()) {
    if (!computed->expr()->Is<googlesql::ResolvedAggregateFunctionCall>()) {
      return Unsupported(scope, "differential privacy aggregate");
    }
    const auto& call = *computed->expr()->GetAs<googlesql::ResolvedAggregateFunctionCall>();
    const auto function = Function(call);
    if (!function) {
      return Unsupported(scope, "differentially private " + call.function()->SQLName() + "(" +
                                    call.argument_list(0)->type()->DebugString() + ")");
    }
    if (call.distinct() || call.where_expr() != nullptr || call.having_modifier() != nullptr ||
        call.limit() != nullptr || !call.order_by_item_list().empty() ||
        call.null_handling_modifier() !=
            googlesql::ResolvedNonScalarFunctionCallBase::DEFAULT_NULL_HANDLING) {
      return Unsupported(scope, "differentially private aggregate with modifiers");
    }
    const absl::StatusOr<double> epsilon = (*assigner)->GetEpsilonForFunction(&call);
    Check(epsilon.status());
    const auto bounds = Bounds(call, scope);
    const auto value = Expression(*call.argument_list(0), scope, input->columns);
    const auto type = DuckDbType(call.type(), nullptr, scope.context.timestamp_precision);
    const auto epsilon_sql =
        Literal(googlesql::Value::Double(*epsilon), scope.context.timestamp_precision);
    if (!bounds || !value || !type || !epsilon_sql) {
      return std::nullopt;
    }
    const int id = computed->column().column_id();
    if (id == threshold_column) {
      threshold_epsilon = *epsilon;
    }
    projections.push_back("CAST(" + std::string(*function) + "(list_filter(list(" + *value +
                          "), lambda _v: _v IS NOT NULL), " + bounds->first + ", " +
                          bounds->second + ", " + *epsilon_sql + ", " + max_groups_sql + ") AS " +
                          *type + ") AS " + ColumnName(id));
  }
  if (!threshold_epsilon) {
    return Unsupported(scope, "differential privacy group selection");
  }
  // Each group's noisy count of privacy units has to reach the threshold that delta implies.
  const absl::StatusOr<googlesql::Value> threshold =
      googlesql::anonymization::ComputeLaplaceThresholdFromDelta(
          googlesql::Value::Double(*threshold_epsilon), *delta,
          bounded ? *max_groups_contributed : googlesql::Value());
  Check(threshold.status());
  const std::string grouped = "SELECT " + Join(projections, ", ") + input->From() +
                              (keys.empty() ? "" : " GROUP BY " + Join(keys, ", "));
  Relation result;
  std::vector<std::string> outputs;
  for (const googlesql::ResolvedColumn& column : aggregate.column_list()) {
    const int id = column.column_id();
    outputs.push_back("q." + ColumnName(id) + " AS " + ColumnName(id));
    result.columns.emplace(id, "q." + ColumnName(id));
  }
  result.sql = "SELECT " + (outputs.empty() ? "1 AS _unit" : Join(outputs, ", ")) + " FROM (" +
               grouped + ") AS q WHERE q." + ColumnName(threshold_column) +
               " >= " + std::to_string(threshold->int64_value());
  return result;
}

}  // namespace bigquery_emulator_duckdb::translator
