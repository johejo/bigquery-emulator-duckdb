#include "src/translator/gap_fill.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <vector>

#include "absl/strings/ascii.h"
#include "absl/strings/match.h"
#include "googlesql/public/table_valued_function.h"
#include "googlesql/public/type.h"
#include "googlesql/public/value.h"
#include "googlesql/resolved_ast/resolved_ast.h"
#include "src/duckdb_sql.h"
#include "src/timestamp_precision.h"
#include "src/translator/context.h"
#include "src/translator/expression.h"
#include "src/translator/scan.h"
#include "src/type_mapping.h"

namespace bigquery_emulator_duckdb::translator {
namespace {

constexpr int64_t kMicrosPerDay = int64_t{86400} * 1000 * 1000;

// Time points are HUGEINT microseconds since the Unix epoch.
struct TimeKey {
  const googlesql::Type* type;

  [[nodiscard]] std::string Key(const std::string& sql) const {
    return "CAST(epoch_us(CAST(" + sql + " AS " +
           (type->IsTimestamp() ? "TIMESTAMPTZ" : "TIMESTAMP") + ")) AS HUGEINT)";
  }
  [[nodiscard]] std::string Value(const std::string& key) const {
    const std::string micros = "CAST(" + key + " AS BIGINT)";
    if (type->IsTimestamp()) {
      return "(TIMESTAMPTZ '1970-01-01 00:00:00+00' + to_microseconds(" + micros + "))";
    }
    if (type->IsDate()) {
      return "CAST(make_timestamp(" + micros + ") AS DATE)";
    }
    return "make_timestamp(" + micros + ")";
  }
  // 1950-01-01 at midnight, UTC for TIMESTAMP.
  [[nodiscard]] static std::string DefaultOrigin() {
    return std::to_string(int64_t{-631152000} * 1000 * 1000);
  }
};

// The number of whole buckets of `width` in the HUGEINT `sql`, rounded down.
std::string FloorDiv(const std::string& sql, const std::string& width) {
  return std::format("(({0}) - ((({0}) % {1}) + {1}) % {1}) // {1}", sql, width);
}

// The literal of the scalar argument `i`, absent when it is omitted or NULL.
std::optional<googlesql::Value> Argument(const googlesql::ResolvedTVFScan& tvf, size_t i) {
  const auto& arguments = tvf.signature()->input_arguments();
  if (i >= arguments.size()) {
    return std::nullopt;
  }
  const auto scalar = arguments.at(i).GetScalarArgType();
  if (!scalar.ok() || scalar->literal_value() == nullptr || scalar->literal_value()->is_null()) {
    return std::nullopt;
  }
  return *scalar->literal_value();
}

}  // namespace

std::optional<Relation> GapFillScan(const googlesql::ResolvedTVFScan& tvf, const Scope& scope) {
  const auto& argument = *tvf.argument_list(0);
  const auto input = Scan(*argument.scan(), scope);
  if (!input) {
    return std::nullopt;
  }
  const auto& schema = tvf.signature()->input_arguments().at(0).relation();
  const auto& result = tvf.signature()->result_schema();
  // The input column of each result column; the catalog validated their names.
  std::vector<std::string> sources;
  for (int i = 0; i < result.num_columns(); ++i) {
    for (int j = 0; j < schema.num_columns(); ++j) {
      if (absl::EqualsIgnoreCase(result.column(i).name, schema.column(j).name)) {
        const auto& column = argument.argument_column_list(j);
        if (!DuckDbType(column.type(), scope.context.timestamp_precision) ||
            column.type_annotation_map() != nullptr) {
          return Unsupported(scope, "GAP_FILL column type");
        }
        sources.push_back("q." + ColumnName(column.column_id()));
      }
    }
  }
  if (result.column(0).type->IsTimestamp() &&
      scope.context.timestamp_precision == TimestampPrecision::kNanoseconds) {
    return Unsupported(scope, "GAP_FILL with nanosecond timestamps");
  }
  const TimeKey time{result.column(0).type};
  // An omitted origin is a NULL literal, as an explicit NULL is.
  const auto given = [&](int i) -> const googlesql::ResolvedExpr* {
    if (i >= tvf.argument_list_size() || tvf.argument_list(i)->expr() == nullptr) {
      return nullptr;
    }
    const auto* expr = tvf.argument_list(i)->expr();
    return expr->Is<googlesql::ResolvedLiteral>() &&
                   expr->GetAs<googlesql::ResolvedLiteral>()->value().is_null()
               ? nullptr
               : expr;
  };
  // BigQuery takes an INTERVAL literal, such as INTERVAL 1 DAY, whose parts GoogleSQL resolves as
  // arguments of $interval, and fails on any other expression, such as -INTERVAL 1 DAY.
  const auto* width_expr = given(2);
  const auto literal = [](const googlesql::ResolvedExpr& expr) {
    if (expr.Is<googlesql::ResolvedLiteral>()) {
      return true;
    }
    const auto* call = expr.GetAs<googlesql::ResolvedFunctionCall>();
    return expr.Is<googlesql::ResolvedFunctionCall>() && call->function()->Name() == "$interval" &&
           std::ranges::all_of(call->argument_list(), [](const auto& argument) {
             return argument->template Is<googlesql::ResolvedLiteral>();
           });
  };
  std::optional<std::string> interval = "CAST(NULL AS INTERVAL)";
  if (width_expr != nullptr && !literal(*width_expr)) {
    interval = "CAST(error('Argument 3 must be a valid INTERVAL literal') AS INTERVAL)";
  } else if (width_expr != nullptr) {
    interval = Expression(*width_expr, scope, {});
  }
  if (!interval) {
    return std::nullopt;
  }
  // BigQuery requires exactly one non-zero, positive part, of months only for DATE and DATETIME.
  const auto* origin_expr = given(4);
  const std::string months = "(datepart('year', _gf_i) * 12 + datepart('month', _gf_i))";
  const std::string days = "datepart('day', _gf_i)";
  const std::string micros =
      "(datepart('hour', _gf_i) * 3600000000 + datepart('minute', _gf_i) * 60000000 + "
      "datepart('microseconds', _gf_i))";
  std::string type_check;
  if (time.type->IsDate()) {
    type_check = "WHEN " + micros +
                 " <> 0 THEN error('Only MONTH and DAY parts are allowed in bucket width "
                 "INTERVAL') ";
  } else if (time.type->IsTimestamp()) {
    type_check = "WHEN " + months +
                 " <> 0 THEN error('Bucket width INTERVAL with non-zero MONTH part is not "
                 "allowed') ";
  }
  // The width in microseconds, w, or in months, m; the other is zero.
  const std::string checked_width = std::format(
      "SELECT CASE WHEN _gf_i IS NULL THEN error('Argument 3 must be non-NULL') WHEN "
      "({0} <> 0)::INT + ({1} <> 0)::INT + ({2} <> 0)::INT <> 1 THEN error('Exactly one non-zero "
      "INTERVAL part in bucket width is required') {3}WHEN {0} < 0 OR {1} < 0 OR {2} < 0 THEN "
      "error('Negative bucket width INTERVAL is not allowed') ELSE struct_pack(w := CAST({1} * {4} "
      "+ {2} AS HUGEINT), m := CAST({0} AS BIGINT)) END AS _gf_s FROM (SELECT {5} AS _gf_i) AS q",
      months, days, micros, type_check, kMicrosPerDay, *interval);
  const auto partitions = Argument(tvf, 5);
  const size_t partition_count = !partitions ? 0 : partitions->num_elements();
  const auto values = Argument(tvf, 3);
  std::string origin = TimeKey::DefaultOrigin();
  if (origin_expr != nullptr) {
    const auto value = Expression(*origin_expr, scope, {});
    if (!value) {
      return std::nullopt;
    }
    origin = "(" + time.Key(*value) + ")";
  }
  const auto ignore = Argument(tvf, 6);
  const bool ignore_nulls = !ignore || ignore->bool_value();

  std::vector<std::string> keys;
  std::vector<std::string> data = {};
  std::vector<std::string> row;
  for (size_t i = 1; i < sources.size(); ++i) {
    const std::string name = "_gf_" + std::to_string(i);
    data.push_back(sources.at(i) + " AS " + name);
    if (i <= partition_count) {
      keys.push_back(name);
    } else {
      row.push_back(IntervalKey(result.column(static_cast<int>(i)).type, name));
    }
  }
  const std::string partition = keys.empty() ? "" : "PARTITION BY " + Join(keys, ", ") + " ";
  const std::string key_list = keys.empty() ? "" : Join(keys, ", ") + ", ";
  // A time point's rows must have equal values, which are condensed into one observation.
  const std::string condensed = row.empty() ? "" : "row(" + Join(row, ", ") + ")";
  data.push_back(time.Key(sources.at(0)) + " AS _gf_raw");
  // Rows without a time point are ignored.
  const std::string observed = std::format(
      "SELECT * EXCLUDE (_gf_raw), {} AS _gf_t FROM (SELECT {}{}) AS q WHERE _gf_raw IS NOT NULL",
      condensed.empty()
          ? "_gf_raw"
          : std::format("CASE WHEN min({0}) OVER (PARTITION BY {1}_gf_raw) IS DISTINCT FROM "
                        "max({0}) OVER (PARTITION BY {1}_gf_raw) THEN error('The emulator does "
                        "not support GAP_FILL different values at one time point') ELSE _gf_raw "
                        "END",
                        condensed, key_list),
      Join(data, ", "), input->From());
  // Buckets lie at the origin plus a multiple of the width, from the earliest to the latest
  // time point of each partition. A month bucket is the origin plus whole months, on the last day
  // of a month shorter than the origin's day; the range of months may include points outside the
  // time points, which are left out.
  const auto timestamp = [](const std::string& key) {
    return "make_timestamp(CAST(" + key + " AS BIGINT))";
  };
  const auto month = [&](const std::string& key) {
    return std::format("(year({0}) * 12 + month({0}))", timestamp(key));
  };
  const std::string origin_month = month("(" + origin + ")");
  const std::string first =
      std::format("CASE WHEN _gf_s.m = 0 THEN -{} ELSE {} END",
                  FloorDiv("(" + origin + ") - min(_gf_t)", "_gf_s.w"),
                  FloorDiv(month("min(_gf_t)") + " - " + origin_month, "_gf_s.m"));
  const std::string last =
      std::format("CASE WHEN _gf_s.m = 0 THEN {} ELSE {} END",
                  FloorDiv("max(_gf_t) - (" + origin + ")", "_gf_s.w"),
                  FloorDiv(month("max(_gf_t)") + " - " + origin_month, "_gf_s.m"));
  const std::string grid = std::format(
      "SELECT {0}_gf_t, 1 AS _gf_kind FROM (SELECT {0}CASE WHEN _gf_s.m = 0 THEN ({1}) + _gf_k * "
      "_gf_s.w ELSE {2} END AS _gf_t, _gf_min, _gf_max FROM (SELECT {0}CAST(unnest(range("
      "CAST(_gf_first AS BIGINT), CAST(_gf_last AS BIGINT) + 1)) AS HUGEINT) AS _gf_k, _gf_s, "
      "_gf_min, _gf_max FROM (SELECT {0}{3} AS _gf_first, {4} AS _gf_last, min(_gf_t) AS _gf_min, "
      "max(_gf_t) AS _gf_max, _gf_s FROM _gf_observed, _gf_width GROUP BY {0}_gf_s) AS q) AS q) "
      "AS q WHERE _gf_t BETWEEN _gf_min AND _gf_max",
      key_list, origin,
      time.Key(timestamp("(" + origin + ")") + " + to_months(CAST(_gf_k * _gf_s.m AS INTEGER))"),
      first, last);
  const std::string ordered = partition + "ORDER BY _gf_t, _gf_kind ";
  const std::string preceding =
      "OVER (" + ordered + "ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW)";
  const std::string following =
      "OVER (" + ordered + "ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING)";
  std::vector<std::string> windows;
  std::vector<std::string> outputs = {time.Value("_gf_t")};
  for (size_t i = 1; i < sources.size(); ++i) {
    const std::string name = "_gf_" + std::to_string(i);
    std::string method = "null";
    if (i > partition_count && values) {
      method = absl::AsciiStrToLower(
          values->element(static_cast<int>(i - 1 - partition_count)).field(1).string_value());
    }
    std::string output = name;
    // An observation is a data row, which skips NULL values when ignore_null_values is TRUE.
    const std::string observation =
        std::format("CASE WHEN _gf_kind = 0{} THEN struct_pack(t := _gf_t, v := {}) END",
                    ignore_nulls ? " AND " + name + " IS NOT NULL" : "", name);
    if (i <= partition_count) {
      windows.push_back(name);
    } else if (method == "null") {
      windows.push_back(std::format(
          "(max(CASE WHEN _gf_kind = 0 THEN struct_pack(v := {0}) END) OVER (PARTITION BY "
          "{1}_gf_t)).v AS {0}",
          name, key_list));
    } else if (method == "locf") {
      windows.push_back(
          std::format("(last_value({} IGNORE NULLS) {}).v AS {}", observation, preceding, name));
    } else {
      windows.push_back(
          std::format("last_value({} IGNORE NULLS) {} AS {}_p", observation, preceding, name));
      windows.push_back(
          std::format("first_value({} IGNORE NULLS) {} AS {}_n", observation, following, name));
      const std::string p = name + "_p";
      const std::string n = name + "_n";
      std::string interpolated;
      if (result.column(static_cast<int>(i)).type->IsDouble()) {
        interpolated = std::format(
            "{0}.v + ({1}.v - {0}.v) * CAST(_gf_t - {0}.t AS DOUBLE) / CAST({1}.t - {0}.t AS "
            "DOUBLE)",
            p, n);
      } else {
        // An INT64 rounds to the nearest integer, and a halfway value toward the later point's
        // value.
        const std::string numerator = std::format(
            "(CAST({0}.v AS HUGEINT) * ({1}.t - _gf_t) + CAST({1}.v AS HUGEINT) * "
            "(_gf_t - {0}.t))",
            p, n);
        const std::string denominator = std::format("({}.t - {}.t)", n, p);
        const std::string remainder =
            std::format("((({0} % {1}) + {1}) % {1})", numerator, denominator);
        interpolated = std::format(
            "CAST(({0} - {2}) // {1} + CASE WHEN 2 * {2} > {1} THEN 1 WHEN 2 * {2} < {1} THEN 0 "
            "WHEN {4}.v > {3}.v THEN 1 ELSE 0 END AS BIGINT)",
            numerator, denominator, remainder, p, n);
      }
      output = std::format(
          "CASE WHEN {0}.t = _gf_t THEN {0}.v WHEN {0}.v IS NULL OR {1}.v IS NULL THEN NULL ELSE "
          "{2} END",
          p, n, interpolated);
    }
    outputs.push_back(output);
  }
  Relation relation;
  std::vector<std::string> projections;
  for (int i = 0; i < tvf.column_list_size(); ++i) {
    const std::string name = ColumnName(tvf.column_list(i).column_id());
    projections.push_back(outputs.at(tvf.column_index_list(i)) + " AS " + name);
    relation.columns.emplace(tvf.column_list(i).column_id(), "q." + name);
  }
  if (projections.empty()) {
    projections.emplace_back("1 AS _unit");
  }
  relation.sql = std::format(
      "WITH _gf_width AS ({}), _gf_observed AS ({}) SELECT {} FROM (SELECT _gf_kind, _gf_t, {} "
      "FROM (SELECT *, 0 AS "
      "_gf_kind FROM _gf_observed UNION ALL BY NAME {}) AS q) AS q WHERE _gf_kind = 1",
      checked_width, observed, Join(projections, ", "), Join(windows, ", "), grid);
  return relation;
}

}  // namespace bigquery_emulator_duckdb::translator
