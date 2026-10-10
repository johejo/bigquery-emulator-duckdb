#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "src/translator/functions.h"
#include "src/translator/functions_internal.h"

namespace bigquery_emulator_duckdb::translator {
namespace {

// histogram() leaves out NULL, which APPROX_TOP_COUNT counts as a value of its own. Sorting the
// (count, value) structs descending puts the most frequent first, and no rows give NULL.
std::string TopCount(const std::string& sql, const std::vector<std::string>& arguments,
                     const std::string& tail) {
  const std::string rows = "count(*)" + tail;
  const std::string nulls = rows + " - count(" + arguments.at(0) + ")" + tail;
  return "CASE WHEN " + rows + " > 0 THEN list_transform(list_slice(list_sort(list_concat(" +
         "list_transform(map_entries(" + sql +
         "), lambda e: {'count': e.value::BIGINT, 'value': e.key}), CASE WHEN " + nulls +
         " > 0 THEN [{'count': " + nulls + ", 'value': NULL}] END), 'DESC'), 1, " +
         arguments.at(1) + "), lambda e: {'value': e.value, 'count': e.count}) END";
}

std::string Unhex(const std::string& sql, const std::vector<std::string>& /*arguments*/,
                  const std::string& /*tail*/) {
  return "unhex(" + sql + ")";
}

std::string Flatten(const std::string& sql, const std::vector<std::string>& /*arguments*/,
                    const std::string& /*tail*/) {
  return "flatten(" + sql + ")";
}

// DuckDB sums BIGNUM exactly, and the sum then has to fit a BIGNUMERIC.
std::string BigNumericSum(const std::string& sql, const std::vector<std::string>& /*arguments*/,
                          const std::string& /*tail*/) {
  return "CAST(bq_bignumeric_sum(CAST(" + sql + " AS VARCHAR)) AS BIGNUM)";
}

// A BIGNUMERIC aggregate that `kFunction`, of src/backend_functions/bignumeric.cc, computes as
// GoogleSQL does from the units of the non-NULL values that list() collects. A pair of arguments
// is collected as the units of both joined by a comma, which is NULL where either is NULL.
template <const char* kFunction>
AggregateRule BigNumericAggregate(std::size_t arity) {
  return {
      .function = "list",
      .type = googlesql::TYPE_BIGNUMERIC,
      .arguments =
          {
              arity == 1 ? "CAST($1 AS VARCHAR)"
                         : "CAST($1 AS VARCHAR) || ',' || CAST($2 AS VARCHAR)",
          },
      .skip_nulls = true,
      .finish =
          [](const std::string& sql, const std::vector<std::string>& /*arguments*/,
             const std::string& /*tail*/) { return std::string(kFunction) + "(" + sql + ")"; },
  };
}

constexpr char kBigNumericAvg[] = "bq_bignumeric_avg";
constexpr char kBigNumericStdDevSamp[] = "bq_bignumeric_stddev_samp";
constexpr char kBigNumericStdDevPop[] = "bq_bignumeric_stddev_pop";
constexpr char kBigNumericVarSamp[] = "bq_bignumeric_var_samp";
constexpr char kBigNumericVarPop[] = "bq_bignumeric_var_pop";
constexpr char kBigNumericCorr[] = "bq_bignumeric_corr";
constexpr char kBigNumericCovarPop[] = "bq_bignumeric_covar_pop";
constexpr char kBigNumericCovarSamp[] = "bq_bignumeric_covar_samp";

// Aggregate functions, which also take a window.
}  // namespace

const std::unordered_map<std::string_view, std::vector<AggregateRule>>& Aggregates() {
  using enum AggregateRule::Nulls;
  using enum AggregateRule::Limit;
  static const auto* const kAggregates =
      new std::unordered_map<std::string_view, std::vector<AggregateRule>>{
          {"COUNT", {{.function = "count"}}},
          {"$COUNT_STAR", {{.function = "count", .arguments = {"*"}}}},
          {
              "SUM",
              {
                  {.function = "sum", .type = googlesql::TYPE_BIGNUMERIC, .finish = BigNumericSum},
                  {.function = "sum"},
              },
          },
          {"AVG", {BigNumericAggregate<kBigNumericAvg>(1), {.function = "avg"}}},
          {"MIN", {{.function = "min"}}},
          {"MAX", {{.function = "max"}}},
          {"ANY_VALUE", {{.function = "any_value"}}},
          {"ARRAY_AGG", {{.function = "list", .nulls = kFilter, .limit = kSlice}}},
          // ARRAY_CONCAT_AGG skips NULL arrays.
          {
              "ARRAY_CONCAT_AGG",
              {{.function = "list", .limit = kSlice, .skip_nulls = true, .finish = Flatten}},
          },
          // Exact, which is within any approximation error.
          {
              "APPROX_COUNT_DISTINCT",
              {{.function = "count", .distinct = AggregateRule::Distinct::kAlways}},
          },
          // APPROX_QUANTILES(x, n) takes the n + 1 quantiles 0, 1/n, ..., 1. quantile_disc()
          // skips NULLs, as APPROX_QUANTILES does by default.
          {
              "APPROX_QUANTILES",
              {
                  {
                      .function = "quantile_disc",
                      .arguments = {"$1", "list_transform(range($2 + 1), lambda i: i / $2)"},
                      .nulls = kIgnore,
                  },
              },
          },
          // histogram() takes only the values; TopCount() applies the count afterwards.
          {
              "APPROX_TOP_COUNT",
              {
                  {
                      .function = "histogram",
                      .arguments = {"$1"},
                      .distinct = AggregateRule::Distinct::kUnsupported,
                      .finish = TopCount,
                  },
              },
          },
          // The _null variants return a NULL x instead of skipping its row.
          {"MAX_BY", {{.function = "arg_max_null"}}},
          {"MIN_BY", {{.function = "arg_min_null"}}},
          // DuckDB's string_agg() only joins strings, so STRING_AGG over BYTES joins their
          // hexadecimal digits; the default delimiter is b','.
          {
              "STRING_AGG",
              {
                  {.function = "string_agg", .type = googlesql::TYPE_STRING, .limit = kJoin},
                  {
                      .function = "string_agg",
                      .type = googlesql::TYPE_BYTES,
                      .arguments = {"hex($1)", "hex($2)"},
                      .defaults = {"", "unhex('2C')"},
                      .limit = kJoin,
                      .finish = Unhex,
                  },
              },
          },
          {"COUNTIF", {{.function = "count_if"}}},
          {"LOGICAL_AND", {{.function = "bool_and"}}},
          {"LOGICAL_OR", {{.function = "bool_or"}}},
          {"BIT_AND", {{.function = "bit_and"}}},
          {"BIT_OR", {{.function = "bit_or"}}},
          {"BIT_XOR", {{.function = "bit_xor"}}},
          {"STDDEV", {BigNumericAggregate<kBigNumericStdDevSamp>(1), {.function = "stddev_samp"}}},
          {
              "STDDEV_SAMP",
              {BigNumericAggregate<kBigNumericStdDevSamp>(1), {.function = "stddev_samp"}},
          },
          {
              "STDDEV_POP",
              {BigNumericAggregate<kBigNumericStdDevPop>(1), {.function = "stddev_pop"}},
          },
          {"VARIANCE", {BigNumericAggregate<kBigNumericVarSamp>(1), {.function = "var_samp"}}},
          {"VAR_SAMP", {BigNumericAggregate<kBigNumericVarSamp>(1), {.function = "var_samp"}}},
          {"VAR_POP", {BigNumericAggregate<kBigNumericVarPop>(1), {.function = "var_pop"}}},
          {"CORR", {BigNumericAggregate<kBigNumericCorr>(2), {.function = "corr"}}},
          {"COVAR_POP", {BigNumericAggregate<kBigNumericCovarPop>(2), {.function = "covar_pop"}}},
          {
              "COVAR_SAMP",
              {BigNumericAggregate<kBigNumericCovarSamp>(2), {.function = "covar_samp"}},
          },
  };
  return *kAggregates;
}

namespace {

// PERCENTILE_CONT and PERCENTILE_DISC read their result off the partition's values, which
// list() sorts as GoogleSQL does: NULLs first, then NaNs, then the other values.
constexpr std::string_view kPercentileOrder = "$1 NULLS FIRST";
constexpr std::string_view kFloatPercentileOrder = "NOT isnan($1) NULLS FIRST, $1 NULLS FIRST";

// GoogleSQL's PERCENTILE_CONT over the sorted list `sql`, of FLOAT64 or, with `kNumeric`, of
// NUMERIC values.
template <bool kNumeric>
std::string PercentileCont(const std::string& sql, const std::vector<std::string>& arguments,
                           const std::string& /*tail*/) {
  return std::string(kNumeric ? "bq_percentile_cont_numeric(" : "bq_percentile_cont(") + sql +
         ", " + arguments.at(1) + ")";
}

// The element of the sorted list `sql` at the position GoogleSQL's PERCENTILE_DISC takes, for a
// percentile of type `kPercentile`: FLOAT64, NUMERIC or BIGNUMERIC, which crosses as the VARCHAR
// of its units.
template <googlesql::TypeKind kPercentile>
std::string PercentileDisc(const std::string& sql, const std::vector<std::string>& arguments,
                           const std::string& /*tail*/) {
  const std::string percentile = kPercentile == googlesql::TYPE_BIGNUMERIC
                                     ? "CAST(" + arguments.at(1) + " AS VARCHAR)"
                                     : arguments.at(1);
  const std::string_view position =
      kPercentile == googlesql::TYPE_NUMERIC      ? "bq_percentile_disc_position_numeric"
      : kPercentile == googlesql::TYPE_BIGNUMERIC ? "bq_percentile_disc_position_bignumeric"
                                                  : "bq_percentile_disc_position";
  return "list_extract(" + sql + ", " + std::string(position) + "(len(" + sql + "), " + percentile +
         "))";
}

// PERCENTILE_DISC of values of `type`, for a percentile of `kPercentile`.
template <googlesql::TypeKind kPercentile>
AggregateRule PercentileDiscRule(googlesql::TypeKind type) {
  return {
      .function = "list",
      .type = type,
      .conditions = {{.argument = 2, .types = {kPercentile}}},
      .arguments = {"$1"},
      .order = type == googlesql::TYPE_DOUBLE ? kFloatPercentileOrder : kPercentileOrder,
      .nulls = AggregateRule::Nulls::kFilterUnlessRespected,
      .finish = PercentileDisc<kPercentile>,
  };
}

// Functions that only take a window.
}  // namespace

const std::unordered_map<std::string_view, std::vector<AggregateRule>>& Analytics() {
  using enum AggregateRule::Nulls;
  static const auto* const kAnalytics =
      new std::unordered_map<std::string_view, std::vector<AggregateRule>>{
          {"ROW_NUMBER", {{.function = "row_number"}}},
          {"RANK", {{.function = "rank"}}},
          {"DENSE_RANK", {{.function = "dense_rank"}}},
          {"PERCENT_RANK", {{.function = "percent_rank"}}},
          {"CUME_DIST", {{.function = "cume_dist"}}},
          {"NTILE", {{.function = "ntile"}}},
          {"LAG", {{.function = "lag"}}},
          {"LEAD", {{.function = "lead"}}},
          {"FIRST_VALUE", {{.function = "first_value", .nulls = kModifier}}},
          {"LAST_VALUE", {{.function = "last_value", .nulls = kModifier}}},
          {"NTH_VALUE", {{.function = "nth_value", .nulls = kModifier}}},
          {
              "PERCENTILE_CONT",
              {
                  {
                      .function = "list",
                      .type = googlesql::TYPE_DOUBLE,
                      .arguments = {"$1"},
                      .order = kFloatPercentileOrder,
                      .nulls = kFilterUnlessRespected,
                      .finish = PercentileCont<false>,
                  },
                  {
                      .function = "list",
                      .type = googlesql::TYPE_NUMERIC,
                      .arguments = {"$1"},
                      .order = kPercentileOrder,
                      .nulls = kFilterUnlessRespected,
                      .finish = PercentileCont<true>,
                  },
              },
          },
          {
              "PERCENTILE_DISC",
              {
                  PercentileDiscRule<googlesql::TYPE_DOUBLE>(googlesql::TYPE_DOUBLE),
                  PercentileDiscRule<googlesql::TYPE_NUMERIC>(googlesql::TYPE_DOUBLE),
                  PercentileDiscRule<googlesql::TYPE_BIGNUMERIC>(googlesql::TYPE_DOUBLE),
                  PercentileDiscRule<googlesql::TYPE_DOUBLE>(googlesql::TYPE_UNKNOWN),
                  PercentileDiscRule<googlesql::TYPE_NUMERIC>(googlesql::TYPE_UNKNOWN),
                  PercentileDiscRule<googlesql::TYPE_BIGNUMERIC>(googlesql::TYPE_UNKNOWN),
              },
          },
  };
  return *kAnalytics;
}

}  // namespace bigquery_emulator_duckdb::translator
