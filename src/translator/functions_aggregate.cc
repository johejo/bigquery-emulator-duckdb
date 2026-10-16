#include "src/translator/functions_aggregate.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "src/translator/functions.h"

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

// Collect each argument once, then let DuckDB group the original values (including NULL,
// NaN, arrays and structs). Exact sums and top values need no approximation algorithm.
// Validation precedes the top-N selection, including weights of values outside the result.
template <googlesql::TypeKind kWeight>
std::string TopSum(const std::string& sql, const std::vector<std::string>& arguments,
                   const std::string& /*tail*/) {
  const std::string weight = "r.weight";
  const std::string invalid =
      weight + " < 0" + (kWeight == googlesql::TYPE_DOUBLE ? " OR isnan(" + weight + ")" : "");
  const std::string display = kWeight == googlesql::TYPE_BIGNUMERIC
                                  ? "bq_bignumeric_to_string(CAST(" + weight + " AS VARCHAR))"
                                  : "lower(CAST(" + weight + " AS VARCHAR))";
  const std::string checked = "CASE WHEN " + invalid + " THEN " +
                              Raise(
                                  "'APPROX_TOP_SUM does not support negative or NaN weights in the "
                                  "second argument; got ' || " +
                                      display,
                                  false) +
                              " ELSE " + weight + " END";
  std::string sum = "sum(" + checked + ")";
  if constexpr (kWeight == googlesql::TYPE_INT64) {
    sum = "CAST(" + sum + " AS BIGINT)";
  } else if constexpr (kWeight == googlesql::TYPE_NUMERIC) {
    sum = "CASE WHEN " + sum +
          " > CAST('99999999999999999999999999999.999999999' AS DECIMAL(38, 9)) THEN " +
          Raise("'numeric overflow: APPROX_TOP_SUM'", false) + " ELSE " + sum + " END";
  } else if constexpr (kWeight == googlesql::TYPE_BIGNUMERIC) {
    sum = "CAST(bq_bignumeric_sum(CAST(" + sum + " AS VARCHAR)) AS BIGNUM)";
  } else {
    sum = "CASE WHEN isinf(" + sum + ") AND NOT bool_or(isinf(" + weight + ")) THEN " +
          Raise("'double overflow; error in APPROX_TOP_SUM'", false) + " ELSE " + sum + " END";
  }
  const std::string& count = arguments.at(2);
  const std::string limit =
      "CASE WHEN " + count + " IS NULL THEN " +
      Raise("'The second argument to APPROX_TOP_SUM function must not be NULL'", false) + " WHEN " +
      count + " <= 0 THEN " +
      Raise("'The second argument to APPROX_TOP_SUM function must be positive'", false) + " WHEN " +
      count + " > 10000 THEN " +
      Raise("'The last argument to approx_top_sum_shard cannot be greater than 10000'", false) +
      " ELSE " + count + " END";
  return "(SELECT list_slice(list({'value': v, 'sum': s} ORDER BY s DESC NULLS LAST), 1, " + limit +
         ") FROM "
         "(SELECT r.value AS v, " +
         sum + " AS s FROM unnest(" + sql +
         ") AS bq_top_sum_rows(r) GROUP BY r.value) AS bq_top_sum_groups)";
}

template <googlesql::TypeKind kWeight>
AggregateRule TopSumRule() {
  return {
      .function = "list",
      .conditions = {{.argument = 2, .types = {kWeight}}},
      .arguments = {"{'value': $1, 'weight': $2}"},
      .distinct = AggregateRule::Distinct::kUnsupported,
      .finish = TopSum<kWeight>,
      .finish_after_scan = true,
  };
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

// An INTERVAL aggregate that `kFunction`, of src/backend_functions/interval.cc, computes as
// GoogleSQL does from the non-NULL values that list() collects. DuckDB's DISTINCT would equate
// differently spelled values, such as a month and 30 days.
template <const char* kFunction>
AggregateRule IntervalAggregate() {
  return {
      .function = "list",
      .type = googlesql::TYPE_INTERVAL,
      .distinct = AggregateRule::Distinct::kUnsupported,
      .skip_nulls = true,
      .finish =
          [](const std::string& sql, const std::vector<std::string>& /*arguments*/,
             const std::string& /*tail*/) { return std::string(kFunction) + "(" + sql + ")"; },
  };
}

constexpr char kIntervalSum[] = "bq_interval_sum";
constexpr char kIntervalAvg[] = "bq_interval_avg";

// Aggregate functions, which also take a window.
}  // namespace

const std::unordered_map<std::string_view, std::vector<AggregateRule>>& Aggregates() {
  using enum AggregateRule::Nulls;
  using enum AggregateRule::Limit;
  static const auto* const kAggregates =
      new std::unordered_map<std::string_view, std::vector<AggregateRule>>{
          // INTERVAL values compare by their keys; among equal values, MIN and MAX return the
          // first, as BigQuery does.
          {
              "COUNT",
              {
                  {
                      .function = "count",
                      .type = googlesql::TYPE_INTERVAL,
                      .arguments = {"bq_interval_key($1)"},
                  },
                  {.function = "count"},
              },
          },
          {"$COUNT_STAR", {{.function = "count", .arguments = {"*"}}}},
          {
              "SUM",
              {
                  {.function = "sum", .type = googlesql::TYPE_BIGNUMERIC, .finish = BigNumericSum},
                  IntervalAggregate<kIntervalSum>(),
                  {.function = "sum"},
              },
          },
          {
              "AVG",
              {
                  BigNumericAggregate<kBigNumericAvg>(1),
                  IntervalAggregate<kIntervalAvg>(),
                  {.function = "avg"},
              },
          },
          {
              "MIN",
              {
                  {
                      .function = "arg_min",
                      .type = googlesql::TYPE_INTERVAL,
                      .arguments = {"$1", "bq_interval_key($1)"},
                  },
                  {.function = "min"},
              },
          },
          {
              "MAX",
              {
                  {
                      .function = "arg_max",
                      .type = googlesql::TYPE_INTERVAL,
                      .arguments = {"$1", "bq_interval_key($1)"},
                  },
                  {.function = "max"},
              },
          },
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
          {
              "APPROX_TOP_SUM",
              {
                  TopSumRule<googlesql::TYPE_INT64>(),
                  TopSumRule<googlesql::TYPE_NUMERIC>(),
                  TopSumRule<googlesql::TYPE_BIGNUMERIC>(),
                  TopSumRule<googlesql::TYPE_DOUBLE>(),
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
          // COUNT counts only TRUE after converting FALSE and NULL to NULL, and returns 0
          // for empty inputs and window frames. DISTINCT still counts TRUE at most once.
          {"COUNTIF", {{.function = "count", .arguments = {"CASE WHEN $1 THEN TRUE END"}}}},
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

// GoogleSQL's PERCENTILE_CONT over the sorted list `sql` of values of `kType`: FLOAT64, NUMERIC
// or BIGNUMERIC, whose values and percentile cross as the VARCHAR of their units.
template <googlesql::TypeKind kType>
std::string PercentileCont(const std::string& sql, const std::vector<std::string>& arguments,
                           const std::string& /*tail*/) {
  if constexpr (kType == googlesql::TYPE_BIGNUMERIC) {
    return "CAST(bq_percentile_cont_bignumeric(" + sql + ", CAST(" + arguments.at(1) +
           " AS VARCHAR)) AS BIGNUM)";
  }
  return std::string(kType == googlesql::TYPE_NUMERIC ? "bq_percentile_cont_numeric("
                                                      : "bq_percentile_cont(") +
         sql + ", " + arguments.at(1) + ")";
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

// TF_IDF scores the document in its first argument over the dictionary of its window, which
// bq_tf_idf_dictionary builds once.
std::string TfIdf(const std::string& sql, const std::vector<std::string>& arguments,
                  const std::string& /*tail*/) {
  return "bq_tf_idf(" + arguments.at(0) + ", " + sql + ")";
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
                      .finish = PercentileCont<googlesql::TYPE_DOUBLE>,
                  },
                  {
                      .function = "list",
                      .type = googlesql::TYPE_NUMERIC,
                      .arguments = {"$1"},
                      .order = kPercentileOrder,
                      .nulls = kFilterUnlessRespected,
                      .finish = PercentileCont<googlesql::TYPE_NUMERIC>,
                  },
                  {
                      .function = "list",
                      .type = googlesql::TYPE_BIGNUMERIC,
                      .arguments = {"CAST($1 AS VARCHAR)"},
                      .order = kPercentileOrder,
                      .nulls = kFilterUnlessRespected,
                      .finish = PercentileCont<googlesql::TYPE_BIGNUMERIC>,
                  },
              },
          },
          {
              "TF_IDF",
              {
                  {
                      .function = "bq_tf_idf_dictionary",
                      .arguments = {"$1", "$2", "$3"},
                      .defaults = {"", "32000", "5"},
                      .finish = TfIdf,
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
