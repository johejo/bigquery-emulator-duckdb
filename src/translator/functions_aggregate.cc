#include "src/translator/functions_aggregate.h"

#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "src/function_catalog/rules.h"

namespace bigquery_emulator_duckdb::translator {

using function_catalog::AggregateRule;
using function_catalog::ConstantArgument;
using function_catalog::Raise;
using function_catalog::StaticAggregateRule;
using function_catalog::StaticCondition;
using function_catalog::TypeUse;

namespace {

constexpr std::array kIntervalTypes = {googlesql::TYPE_INTERVAL};
constexpr std::array kRangeTypes = {googlesql::TYPE_RANGE};

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

// A BIGNUMERIC aggregate that `kFunction`, of src/googlesql_functions_duckdb/bignumeric.cc,
// computes as GoogleSQL does from the units of the non-NULL values that list() collects. A pair of
// arguments is collected as the units of both joined by a comma, which is NULL where either is
// NULL.
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

// An INTERVAL aggregate that `kFunction`, of src/googlesql_functions_duckdb/interval.cc, computes
// as GoogleSQL does from the non-NULL values that list() collects. DuckDB's DISTINCT would keep any
// of equal values, such as a month and 30 days, so the DISTINCT functions keep the first of them.
template <const char* kFunction>
AggregateRule IntervalAggregate(
    AggregateRule::Distinct distinct = AggregateRule::Distinct::kAllowed) {
  return {
      .function = "list",
      .type = googlesql::TYPE_INTERVAL,
      .distinct = distinct,
      .skip_nulls = true,
      .finish =
          [](const std::string& sql, const std::vector<std::string>& /*arguments*/,
             const std::string& /*tail*/) { return std::string(kFunction) + "(" + sql + ")"; },
  };
}

constexpr char kIntervalSum[] = "bq_interval_sum";
constexpr char kIntervalAvg[] = "bq_interval_avg";
constexpr char kIntervalSumDistinct[] = "bq_interval_sum_distinct";
constexpr char kIntervalAvgDistinct[] = "bq_interval_avg_distinct";

std::string IntervalDistinct(const std::string& sql, const std::vector<std::string>& /*arguments*/,
                             const std::string& /*tail*/) {
  return "bq_interval_distinct(" + sql + ")";
}

// The estimate of the sketch that bq_hll_count_init or bq_hll_count_merge_partial builds, 0 for
// none, as BigQuery's MERGE and APPROX_COUNT_DISTINCT estimate a merged sketch.
std::string ExtractSketch(const std::string& sql, const std::vector<std::string>& /*arguments*/,
                          const std::string& /*tail*/) {
  return "bq_hll_count_extract(" + sql + ")";
}

// HLL_COUNT.INIT over values of `type`, or of any type but BIGNUMERIC for TYPE_UNKNOWN. It takes a
// BIGNUMERIC as the VARCHAR of its units.
AggregateRule HllCountInit(googlesql::TypeKind type) {
  const bool bignumeric = type == googlesql::TYPE_BIGNUMERIC;
  return {
      .function = bignumeric ? "bq_hll_count_init_bignumeric" : "bq_hll_count_init",
      .type = type,
      .arguments = {bignumeric ? "CAST($1 AS VARCHAR)" : "$1", "$2"},
      .defaults = {"", "15"},
      .distinct = AggregateRule::Distinct::kUnsupported,
  };
}

// APPROX_COUNT_DISTINCT over values of `type`, which BigQuery estimates as HLL_COUNT.INIT's
// sketch at its default precision.
AggregateRule ApproxCountDistinct(googlesql::TypeKind type) {
  AggregateRule rule = HllCountInit(type);
  rule.arguments.at(1) = "15";
  rule.distinct = AggregateRule::Distinct::kAllowed;
  rule.finish = ExtractSketch;
  return rule;
}

// Aggregate functions, which also take a window.
}  // namespace

std::span<const function_catalog::NamedDefinition<AggregateRule>> Aggregates() {
  using enum AggregateRule::Nulls;
  using enum AggregateRule::Limit;
  static const function_catalog::NamedDefinition<AggregateRule> kAggregates[] = {
      // INTERVAL values compare by their keys; among equal values, MIN and MAX return the
      // first, as BigQuery does.
      {
          "COUNT",
          {
              .rules =
                  {
                      {
                          .function = "count",
                          .type = googlesql::TYPE_INTERVAL,
                          .arguments = {"bq_interval_key($1)"},
                      },
                      {.function = "count"},
                  },
              .uses = {TypeUse::kAggregatesRange, TypeUse::kBigNumeric},
          },
      },
      {"$COUNT_STAR", {.rules = {{.function = "count", .arguments = {"*"}}}}},
      {
          "SUM",
          {
              .rules =
                  {
                      {
                          .function = "sum",
                          .type = googlesql::TYPE_BIGNUMERIC,
                          .finish = BigNumericSum,
                      },
                      IntervalAggregate<kIntervalSumDistinct>(AggregateRule::Distinct::kRequired),
                      IntervalAggregate<kIntervalSum>(),
                      {.function = "sum"},
                  },
              .uses = {TypeUse::kBigNumeric},
          },
      },
      {
          "AVG",
          {
              .rules =
                  {
                      BigNumericAggregate<kBigNumericAvg>(1),
                      IntervalAggregate<kIntervalAvgDistinct>(AggregateRule::Distinct::kRequired),
                      IntervalAggregate<kIntervalAvg>(),
                      {.function = "avg"},
                  },
              .uses = {TypeUse::kBigNumeric},
          },
      },
      {
          "MIN",
          {
              .rules =
                  {
                      {
                          .function = "arg_min",
                          .type = googlesql::TYPE_INTERVAL,
                          .arguments = {"$1", "bq_interval_key($1)"},
                      },
                      {.function = "min"},
                  },
              .uses = {TypeUse::kAggregatesRange, TypeUse::kBigNumeric},
          },
      },
      {
          "MAX",
          {
              .rules =
                  {
                      {
                          .function = "arg_max",
                          .type = googlesql::TYPE_INTERVAL,
                          .arguments = {"$1", "bq_interval_key($1)"},
                      },
                      {.function = "max"},
                  },
              .uses = {TypeUse::kAggregatesRange, TypeUse::kBigNumeric},
          },
      },
      {
          "ANY_VALUE",
          {
              .rules = {{.function = "any_value", .carries = true}},
              .uses = {TypeUse::kAggregatesRange, TypeUse::kBigNumeric},
          },
      },
      // DuckDB's DISTINCT would keep any of equal INTERVAL values, such as a month and 30
      // days, so the INTERVAL function keeps the first of them, and LIMIT then slices them.
      // STRUCT values with INTERVAL fields are collected as they are.
      {
          "ARRAY_AGG",
          {
              .rules =
                  {
                      {
                          .function = "list",
                          .type = googlesql::TYPE_INTERVAL,
                          .nulls = kFilter,
                          .distinct = AggregateRule::Distinct::kRequired,
                          .limit = kSliceFinished,
                          .finish = IntervalDistinct,
                          .order_by_argument_error = "Unsupported type INTERVAL for array_agg.",
                      },
                      {
                          .function = "list",
                          .type = googlesql::TYPE_INTERVAL,
                          .nulls = kFilter,
                          .limit = kSlice,
                          .order_by_argument_error = "Unsupported type INTERVAL for array_agg.",
                      },
                      {.function = "list", .nulls = kFilter, .limit = kSlice, .carries = true},
                  },
              .uses = {TypeUse::kAggregatesRange, TypeUse::kBigNumeric},
          },
      },
      // ARRAY_CONCAT_AGG skips NULL arrays.
      {
          "ARRAY_CONCAT_AGG",
          {
              .rules =
                  {
                      {
                          .function = "list",
                          .limit = kSlice,
                          .skip_nulls = true,
                          .carries = true,
                          .finish = Flatten,
                      },
                  },
              .uses = {TypeUse::kBigNumeric},
          },
      },
      {
          "HLL_COUNT.INIT",
          {
              .rules =
                  {
                      HllCountInit(googlesql::TYPE_BIGNUMERIC),
                      HllCountInit(googlesql::TYPE_UNKNOWN),
                  },
              .uses = {TypeUse::kBigNumeric, TypeUse::kRejectsOver},
          },
      },
      {
          "HLL_COUNT.MERGE_PARTIAL",
          {
              .rules = {{.function = "bq_hll_count_merge_partial"}},
              .uses = {TypeUse::kRejectsOver},
          },
      },
      {
          "HLL_COUNT.MERGE",
          {
              .rules = {{.function = "bq_hll_count_merge_partial", .finish = ExtractSketch}},
              .uses = {TypeUse::kRejectsOver},
          },
      },
      // Other types count exactly, which is within any approximation error.
      {
          "APPROX_COUNT_DISTINCT",
          {
              .rules =
                  {
                      ApproxCountDistinct(googlesql::TYPE_INT64),
                      ApproxCountDistinct(googlesql::TYPE_STRING),
                      ApproxCountDistinct(googlesql::TYPE_BYTES),
                      ApproxCountDistinct(googlesql::TYPE_NUMERIC),
                      ApproxCountDistinct(googlesql::TYPE_BIGNUMERIC),
                      {.function = "count", .distinct = AggregateRule::Distinct::kAlways},
                  },
              .uses = {TypeUse::kBigNumeric, TypeUse::kRejectsOver},
              .type_rejections =
                  {
                      {
                          .types = kIntervalTypes,
                          .message = "Unsupported type INTERVAL for hll_count.init.",
                      },
                      {
                          .types = kRangeTypes,
                          .message = "Unsupported type RANGE for hll_count.init.",
                      },
                  },
          },
      },
      // APPROX_QUANTILES(x, n) takes the n + 1 quantiles 0, 1/n, ..., 1. quantile_disc()
      // skips NULLs, as APPROX_QUANTILES does by default.
      {
          "APPROX_QUANTILES",
          {
              .rules =
                  {
                      {
                          .function = "quantile_disc",
                          .arguments = {"$1", "list_transform(range($2 + 1), lambda i: i / $2)"},
                          .nulls = kIgnore,
                      },
                  },
              .uses = {TypeUse::kBigNumeric},
              .type_rejections =
                  {
                      {
                          .types = kIntervalTypes,
                          .message =
                              "Unsupported type INTERVAL for aggregate function approx_quantiles",
                      },
                  },
          },
      },
      // histogram() takes only the values; TopCount() applies the count afterwards.
      {
          "APPROX_TOP_COUNT",
          {
              .rules =
                  {
                      {
                          .function = "histogram",
                          .arguments = {"$1"},
                          .distinct = AggregateRule::Distinct::kUnsupported,
                          .finish = TopCount,
                      },
                  },
              .uses = {TypeUse::kBigNumeric},
              .type_rejections =
                  {
                      {
                          .types = kIntervalTypes,
                          .message = "Unsupported type INTERVAL for approx_top_count_shard.",
                      },
                      {
                          .types = kRangeTypes,
                          .message = "Unsupported type RANGE for approx_top_count_shard.",
                      },
                  },
          },
      },
      {
          "APPROX_TOP_SUM",
          {
              .rules =
                  {
                      TopSumRule<googlesql::TYPE_INT64>(),
                      TopSumRule<googlesql::TYPE_NUMERIC>(),
                      TopSumRule<googlesql::TYPE_BIGNUMERIC>(),
                      TopSumRule<googlesql::TYPE_DOUBLE>(),
                  },
              .uses = {TypeUse::kBigNumeric},
              .type_rejections =
                  {
                      {
                          .types = kIntervalTypes,
                          .message = "Unsupported type INTERVAL for approx_top_sum_shard.",
                      },
                      {
                          .types = kRangeTypes,
                          .message = "Unsupported type RANGE for approx_top_sum_shard.",
                      },
                  },
          },
      },
      // The _null variants return a NULL x instead of skipping its row. INTERVAL values of y
      // compare by their keys.
      {
          "MAX_BY",
          {
              .rules =
                  {
                      {
                          .function = "arg_max_null",
                          .conditions = {{.argument = 2, .types = {googlesql::TYPE_INTERVAL}}},
                          .arguments = {"$1", "bq_interval_key($2)"},
                          .carries = true,
                      },
                      {.function = "arg_max_null", .carries = true},
                  },
              .uses = {TypeUse::kAggregatesRange, TypeUse::kBigNumeric},
          },
      },
      {
          "MIN_BY",
          {
              .rules =
                  {
                      {
                          .function = "arg_min_null",
                          .conditions = {{.argument = 2, .types = {googlesql::TYPE_INTERVAL}}},
                          .arguments = {"$1", "bq_interval_key($2)"},
                          .carries = true,
                      },
                      {.function = "arg_min_null", .carries = true},
                  },
              .uses = {TypeUse::kAggregatesRange, TypeUse::kBigNumeric},
          },
      },
      // DuckDB's string_agg() only joins strings, so STRING_AGG over BYTES joins their
      // hexadecimal digits; the default delimiter is b','.
      {
          "STRING_AGG",
          {
              .rules =
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
      },
      // COUNT counts only TRUE after converting FALSE and NULL to NULL, and returns 0
      // for empty inputs and window frames. DISTINCT still counts TRUE at most once.
      {"COUNTIF", {.rules = {{.function = "count", .arguments = {"CASE WHEN $1 THEN TRUE END"}}}}},
      {"LOGICAL_AND", {.rules = {{.function = "bool_and"}}}},
      {"LOGICAL_OR", {.rules = {{.function = "bool_or"}}}},
      {"BIT_AND", {.rules = {{.function = "bit_and"}}}},
      {"BIT_OR", {.rules = {{.function = "bit_or"}}}},
      {"BIT_XOR", {.rules = {{.function = "bit_xor"}}}},
      {
          "STDDEV",
          {
              .rules = {BigNumericAggregate<kBigNumericStdDevSamp>(1), {.function = "stddev_samp"}},
              .uses = {TypeUse::kBigNumeric},
          },
      },
      {
          "STDDEV_SAMP",
          {
              .rules = {BigNumericAggregate<kBigNumericStdDevSamp>(1), {.function = "stddev_samp"}},
              .uses = {TypeUse::kBigNumeric},
          },
      },
      {
          "STDDEV_POP",
          {
              .rules = {BigNumericAggregate<kBigNumericStdDevPop>(1), {.function = "stddev_pop"}},
              .uses = {TypeUse::kBigNumeric},
          },
      },
      {
          "VARIANCE",
          {
              .rules = {BigNumericAggregate<kBigNumericVarSamp>(1), {.function = "var_samp"}},
              .uses = {TypeUse::kBigNumeric},
          },
      },
      {
          "VAR_SAMP",
          {
              .rules = {BigNumericAggregate<kBigNumericVarSamp>(1), {.function = "var_samp"}},
              .uses = {TypeUse::kBigNumeric},
          },
      },
      {
          "VAR_POP",
          {
              .rules = {BigNumericAggregate<kBigNumericVarPop>(1), {.function = "var_pop"}},
              .uses = {TypeUse::kBigNumeric},
          },
      },
      {
          "CORR",
          {
              .rules = {BigNumericAggregate<kBigNumericCorr>(2), {.function = "corr"}},
              .uses = {TypeUse::kBigNumeric},
          },
      },
      {
          "COVAR_POP",
          {
              .rules = {BigNumericAggregate<kBigNumericCovarPop>(2), {.function = "covar_pop"}},
              .uses = {TypeUse::kBigNumeric},
          },
      },
      {
          "COVAR_SAMP",
          {
              .rules = {BigNumericAggregate<kBigNumericCovarSamp>(2), {.function = "covar_samp"}},
              .uses = {TypeUse::kBigNumeric},
          },
      },
  };
  return kAggregates;
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

constexpr std::string_view kValueArgument[] = {"$1"};
constexpr std::string_view kBigNumericValueArgument[] = {"CAST($1 AS VARCHAR)"};

template <googlesql::TypeKind kPercentile>
struct PercentileCondition {
  static constexpr std::array kTypes = {kPercentile};
  static constexpr StaticCondition kConditions[] = {{.argument = 2, .types = kTypes}};
};

// PERCENTILE_DISC of values of `type`, for a percentile of `kPercentile`.
template <googlesql::TypeKind kPercentile>
constexpr StaticAggregateRule PercentileDiscRule(googlesql::TypeKind type) {
  return {
      .function = "list",
      .type = type,
      .conditions = PercentileCondition<kPercentile>::kConditions,
      .arguments = kValueArgument,
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

std::span<const function_catalog::StaticNamedDefinition<StaticAggregateRule>> Analytics() {
  using enum AggregateRule::Nulls;
  static constexpr StaticAggregateRule kRowNumber[] = {{.function = "row_number"}};
  static constexpr StaticAggregateRule kRank[] = {{.function = "rank"}};
  static constexpr StaticAggregateRule kDenseRank[] = {{.function = "dense_rank"}};
  static constexpr StaticAggregateRule kPercentRank[] = {{.function = "percent_rank"}};
  static constexpr StaticAggregateRule kCumeDist[] = {{.function = "cume_dist"}};
  static constexpr StaticAggregateRule kNtile[] = {{.function = "ntile"}};
  static constexpr ConstantArgument kLagConstants[] = {
      {
          .argument = 3,
          .description = "default expression",
          .message = "The third argument (the default expression) to LAG must be constant",
      },
  };
  static constexpr ConstantArgument kLeadConstants[] = {
      {
          .argument = 3,
          .description = "default expression",
          .message = "The third argument (the default expression) to LEAD must be constant",
      },
  };
  static constexpr StaticAggregateRule kLag[] = {
      {.function = "lag", .carries = true, .constant_arguments = kLagConstants},
  };
  static constexpr StaticAggregateRule kLead[] = {
      {.function = "lead", .carries = true, .constant_arguments = kLeadConstants},
  };
  static constexpr StaticAggregateRule kFirstValue[] = {
      {.function = "first_value", .nulls = kModifier, .carries = true},
  };
  static constexpr StaticAggregateRule kLastValue[] = {
      {.function = "last_value", .nulls = kModifier, .carries = true},
  };
  static constexpr StaticAggregateRule kNthValue[] = {
      {.function = "nth_value", .nulls = kModifier, .carries = true},
  };
  static constexpr StaticAggregateRule kPercentileCont[] = {
      {
          .function = "list",
          .type = googlesql::TYPE_DOUBLE,
          .arguments = kValueArgument,
          .order = kFloatPercentileOrder,
          .nulls = kFilterUnlessRespected,
          .finish = PercentileCont<googlesql::TYPE_DOUBLE>,
      },
      {
          .function = "list",
          .type = googlesql::TYPE_NUMERIC,
          .arguments = kValueArgument,
          .order = kPercentileOrder,
          .nulls = kFilterUnlessRespected,
          .finish = PercentileCont<googlesql::TYPE_NUMERIC>,
      },
      {
          .function = "list",
          .type = googlesql::TYPE_BIGNUMERIC,
          .arguments = kBigNumericValueArgument,
          .order = kPercentileOrder,
          .nulls = kFilterUnlessRespected,
          .finish = PercentileCont<googlesql::TYPE_BIGNUMERIC>,
      },
  };
  static constexpr std::string_view kTfIdfArguments[] = {"$1", "$2", "$3"};
  static constexpr std::string_view kTfIdfDefaults[] = {"", "32000", "5"};
  static constexpr StaticAggregateRule kTfIdf[] = {
      {
          .function = "bq_tf_idf_dictionary",
          .arguments = kTfIdfArguments,
          .defaults = kTfIdfDefaults,
          .finish = TfIdf,
      },
  };
  static constexpr StaticAggregateRule kPercentileDisc[] = {
      PercentileDiscRule<googlesql::TYPE_DOUBLE>(googlesql::TYPE_DOUBLE),
      PercentileDiscRule<googlesql::TYPE_NUMERIC>(googlesql::TYPE_DOUBLE),
      PercentileDiscRule<googlesql::TYPE_BIGNUMERIC>(googlesql::TYPE_DOUBLE),
      PercentileDiscRule<googlesql::TYPE_DOUBLE>(googlesql::TYPE_UNKNOWN),
      PercentileDiscRule<googlesql::TYPE_NUMERIC>(googlesql::TYPE_UNKNOWN),
      PercentileDiscRule<googlesql::TYPE_BIGNUMERIC>(googlesql::TYPE_UNKNOWN),
  };
  static constexpr function_catalog::StaticNamedDefinition<StaticAggregateRule> kAnalytics[] = {
      {"ROW_NUMBER", {.rules = kRowNumber}},
      {"RANK", {.rules = kRank}},
      {"DENSE_RANK", {.rules = kDenseRank}},
      {"PERCENT_RANK", {.rules = kPercentRank}},
      {"CUME_DIST", {.rules = kCumeDist}},
      {"NTILE", {.rules = kNtile}},
      {"LAG", {.rules = kLag, .uses = {TypeUse::kAggregatesRange, TypeUse::kBigNumeric}}},
      {"LEAD", {.rules = kLead, .uses = {TypeUse::kAggregatesRange, TypeUse::kBigNumeric}}},
      {
          "FIRST_VALUE",
          {.rules = kFirstValue, .uses = {TypeUse::kAggregatesRange, TypeUse::kBigNumeric}},
      },
      {
          "LAST_VALUE",
          {.rules = kLastValue, .uses = {TypeUse::kAggregatesRange, TypeUse::kBigNumeric}},
      },
      {
          "NTH_VALUE",
          {.rules = kNthValue, .uses = {TypeUse::kAggregatesRange, TypeUse::kBigNumeric}},
      },
      {"PERCENTILE_CONT", {.rules = kPercentileCont, .uses = {TypeUse::kBigNumeric}}},
      {"TF_IDF", {.rules = kTfIdf}},
      {"PERCENTILE_DISC", {.rules = kPercentileDisc, .uses = {TypeUse::kBigNumeric}}},
  };
  return kAnalytics;
}

}  // namespace bigquery_emulator_duckdb::translator
