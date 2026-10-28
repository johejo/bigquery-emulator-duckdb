#include "src/translator/functions_aggregate.h"

#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include "src/function_catalog/rules.h"
#include "src/function_catalog/static_definitions.h"

namespace bigquery_emulator_duckdb::translator {

using function_catalog::Define;
using function_catalog::DefinitionViews;

using function_catalog::AggregateRule;
using function_catalog::ConstantArgument;
using function_catalog::Raise;
using function_catalog::StaticAggregateRule;
using function_catalog::StaticCondition;
using function_catalog::TypeUse;

namespace {

constexpr std::array kIntervalTypes = {googlesql::TYPE_INTERVAL};
constexpr std::array kRangeTypes = {googlesql::TYPE_RANGE};
constexpr std::string_view kValueArgument[] = {"$1"};
constexpr std::string_view kBigNumericValueArgument[] = {"CAST($1 AS VARCHAR)"};
constexpr std::string_view kBigNumericPairArgument[] = {
    "CAST($1 AS VARCHAR) || ',' || CAST($2 AS VARCHAR)",
};
constexpr std::string_view kTopSumArguments[] = {"{'value': $1, 'weight': $2}"};

template <googlesql::TypeKind kType>
struct ArgumentType {
  static constexpr std::array kTypes = {kType};
  static constexpr StaticCondition kSecond[] = {{.argument = 2, .types = kTypes}};
};

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
constexpr StaticAggregateRule TopSumRule() {
  return {
      .function = "list",
      .conditions = ArgumentType<kWeight>::kSecond,
      .arguments = kTopSumArguments,
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
constexpr StaticAggregateRule BigNumericAggregate(std::size_t arity) {
  return {
      .function = "list",
      .type = googlesql::TYPE_BIGNUMERIC,
      .arguments = arity == 1 ? kBigNumericValueArgument : kBigNumericPairArgument,
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
constexpr StaticAggregateRule IntervalAggregate(
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
constexpr std::string_view kHllArguments[] = {"$1", "$2"};
constexpr std::string_view kBigNumericHllArguments[] = {"CAST($1 AS VARCHAR)", "$2"};
constexpr std::string_view kHllDefaults[] = {"", "15"};
constexpr std::string_view kApproxCountArguments[] = {"$1", "15"};
constexpr std::string_view kBigNumericApproxCountArguments[] = {"CAST($1 AS VARCHAR)", "15"};

constexpr StaticAggregateRule HllCountInit(googlesql::TypeKind type) {
  const bool bignumeric = type == googlesql::TYPE_BIGNUMERIC;
  return {
      .function = bignumeric ? "bq_hll_count_init_bignumeric" : "bq_hll_count_init",
      .type = type,
      .arguments = bignumeric ? kBigNumericHllArguments : kHllArguments,
      .defaults = kHllDefaults,
      .distinct = AggregateRule::Distinct::kUnsupported,
  };
}

// APPROX_COUNT_DISTINCT over values of `type`, which BigQuery estimates as HLL_COUNT.INIT's
// sketch at its default precision.
constexpr StaticAggregateRule ApproxCountDistinct(googlesql::TypeKind type) {
  StaticAggregateRule rule = HllCountInit(type);
  rule.arguments =
      type == googlesql::TYPE_BIGNUMERIC ? kBigNumericApproxCountArguments : kApproxCountArguments;
  rule.distinct = AggregateRule::Distinct::kAllowed;
  rule.finish = ExtractSketch;
  return rule;
}

constexpr std::string_view kKllInitArguments[] = {"$1", "$2", "$3"};
constexpr std::string_view kKllInitDefaults[] = {"", "1000", "1"};
constexpr ConstantArgument kKllPrecision[] = {
    {
        .argument = 2,
        .description = "precision",
        .message = "Argument 2 must be a literal or query parameter",
    },
};
constexpr ConstantArgument kKllQuantiles[] = {
    {.argument = 2, .description = "num_quantiles", .message = "Argument #2 must be constant"},
};

template <bool kInteger>
std::string FinishKllInit(const std::string& sql, const std::vector<std::string>& args,
                          const std::string& /*tail*/) {
  const std::string precision = args.size() > 1 ? args.at(1) : "1000";
  const std::string name = kInteger ? "KLL_QUANTILES.INIT_INT64" : "KLL_QUANTILES.INIT_FLOAT64";
  return "CASE WHEN " + precision + " BETWEEN 1 AND 100000 THEN " + sql + " ELSE " +
         Raise("'Provided Precision: ' || coalesce(CAST(" + precision +
                   " AS VARCHAR), 'NULL') || ' but Precision needs to be in the range [1, 100000]; "
                   "error in " +
                   name + " function.'",
               false) +
         " END";
}

constexpr StaticAggregateRule KllInit(bool integer) {
  return {
      .function = integer ? "bq_kll_init_int64" : "bq_kll_init_float64",
      .arguments = kKllInitArguments,
      .defaults = kKllInitDefaults,
      .distinct = AggregateRule::Distinct::kUnsupported,
      .finish = integer ? FinishKllInit<true> : FinishKllInit<false>,
      .constant_arguments = kKllPrecision,
  };
}

template <bool kInteger, bool kPoint>
std::string FinishKllMerge(const std::string& sql, const std::vector<std::string>& args,
                           const std::string& /*tail*/) {
  return std::string(kPoint ? "bq_kll_extract_point_" : "bq_kll_extract_") +
         (kInteger ? "int64" : "float64") + "(" + sql + ", " + args.at(1) + ")";
}

// BigQuery takes none of the KLL aggregates with SAFE, OVER or DISTINCT.
constexpr function_catalog::TypeUses kKllUses = {
    TypeUse::kRejectsSafe,
    TypeUse::kRejectsOver,
    TypeUse::kRejectsDistinct,
};

template <bool kInteger, bool kPoint>
constexpr StaticAggregateRule KllMerge() {
  return {
      .function = kInteger ? "bq_kll_merge_int64" : "bq_kll_merge_float64",
      .arguments = kValueArgument,
      .distinct = AggregateRule::Distinct::kUnsupported,
      .finish = FinishKllMerge<kInteger, kPoint>,
      .constant_arguments = kKllQuantiles,
  };
}

// Aggregate functions, which also take a window.
}  // namespace

std::span<const function_catalog::StaticNamedDefinition<StaticAggregateRule>> Aggregates() {
  using enum AggregateRule::Nulls;
  using enum AggregateRule::Limit;
  static constexpr std::string_view kIntervalKey[] = {"bq_interval_key($1)"};
  static constexpr std::string_view kStar[] = {"*"};
  static constexpr std::string_view kIntervalValueAndKey[] = {"$1", "bq_interval_key($1)"};
  static constexpr std::string_view kIntervalSecondKey[] = {"$1", "bq_interval_key($2)"};
  static constexpr std::string_view kQuantileArguments[] = {
      "$1",
      "list_transform(range($2 + 1), lambda i: i / $2)",
  };
  static constexpr std::string_view kHexArguments[] = {"hex($1)", "hex($2)"};
  static constexpr std::string_view kHexDefaults[] = {"", "unhex('2C')"};
  static constexpr std::string_view kCountIfArguments[] = {"CASE WHEN $1 THEN TRUE END"};
  static constexpr StaticAggregateRule kStddevSamp[] = {
      BigNumericAggregate<kBigNumericStdDevSamp>(1),
      {.function = "stddev_samp"},
  };
  static constexpr StaticAggregateRule kVarSamp[] = {
      BigNumericAggregate<kBigNumericVarSamp>(1),
      {.function = "var_samp"},
  };
  static constexpr function_catalog::TypeRejection kApproxCountRejections[] = {
      {.types = kIntervalTypes, .message = "Unsupported type INTERVAL for hll_count.init."},
      {.types = kRangeTypes, .message = "Unsupported type RANGE for hll_count.init."},
  };
  static constexpr function_catalog::TypeRejection kApproxQuantileRejections[] = {
      {
          .types = kIntervalTypes,
          .message = "Unsupported type INTERVAL for aggregate function approx_quantiles",
      },
  };
  static constexpr function_catalog::TypeRejection kTopCountRejections[] = {
      {.types = kIntervalTypes, .message = "Unsupported type INTERVAL for approx_top_count_shard."},
      {.types = kRangeTypes, .message = "Unsupported type RANGE for approx_top_count_shard."},
  };
  static constexpr function_catalog::TypeRejection kTopSumRejections[] = {
      {.types = kIntervalTypes, .message = "Unsupported type INTERVAL for approx_top_sum_shard."},
      {.types = kRangeTypes, .message = "Unsupported type RANGE for approx_top_sum_shard."},
  };
  static constexpr auto kAggregatesBacking = std::tuple{
      // INTERVAL values compare by their keys; among equal values, MIN and MAX return the
      // first, as BigQuery does.

      Define("COUNT",
             {
                 {
                     .function = "count",
                     .type = googlesql::TYPE_INTERVAL,
                     .arguments = kIntervalKey,
                 },
                 {.function = "count"},
             },
             {
                 .uses = {TypeUse::kAggregatesRange, TypeUse::kBigNumeric, TypeUse::kGraphValues},
             }),
      Define("$COUNT_STAR", {{.function = "count", .arguments = kStar}}),

      Define("SUM",
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
             {
                 .uses = {TypeUse::kBigNumeric},
             }),

      Define("AVG",
             {
                 BigNumericAggregate<kBigNumericAvg>(1),
                 IntervalAggregate<kIntervalAvgDistinct>(AggregateRule::Distinct::kRequired),
                 IntervalAggregate<kIntervalAvg>(),
                 {.function = "avg"},
             },
             {
                 .uses = {TypeUse::kBigNumeric},
             }),

      Define("MIN",
             {
                 {
                     .function = "arg_min",
                     .type = googlesql::TYPE_INTERVAL,
                     .arguments = kIntervalValueAndKey,
                 },
                 {.function = "min"},
             },
             {
                 .uses = {TypeUse::kAggregatesRange, TypeUse::kBigNumeric},
             }),

      Define("MAX",
             {
                 {
                     .function = "arg_max",
                     .type = googlesql::TYPE_INTERVAL,
                     .arguments = kIntervalValueAndKey,
                 },
                 {.function = "max"},
             },
             {
                 .uses = {TypeUse::kAggregatesRange, TypeUse::kBigNumeric},
             }),

      Define("ANY_VALUE", {{.function = "any_value", .carries = true}},
             {
                 .uses = {TypeUse::kAggregatesRange, TypeUse::kBigNumeric},
             }),
      // DuckDB's DISTINCT would keep any of equal INTERVAL values, such as a month and 30
      // days, so the INTERVAL function keeps the first of them, and LIMIT then slices them.
      // STRUCT values with INTERVAL fields are collected as they are.

      Define("ARRAY_AGG",
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
             {
                 .uses = {TypeUse::kAggregatesRange, TypeUse::kBigNumeric},
             }),
      // ARRAY_CONCAT_AGG skips NULL arrays.

      Define("ARRAY_CONCAT_AGG",
             {
                 {
                     .function = "list",
                     .limit = kSlice,
                     .skip_nulls = true,
                     .carries = true,
                     .finish = Flatten,
                 },
             },
             {
                 .uses = {TypeUse::kBigNumeric},
             }),

      Define("KLL_QUANTILES.INIT_INT64", {KllInit(true)}, {.uses = kKllUses}),
      Define("KLL_QUANTILES.INIT_FLOAT64", {KllInit(false)}, {.uses = kKllUses}),
      Define(
          "KLL_QUANTILES.MERGE_PARTIAL",
          {{.function = "bq_kll_merge_partial", .distinct = AggregateRule::Distinct::kUnsupported}},
          {.uses = kKllUses}),
      Define("KLL_QUANTILES.MERGE_INT64", {KllMerge<true, false>()}, {.uses = kKllUses}),
      Define("KLL_QUANTILES.MERGE_FLOAT64", {KllMerge<false, false>()}, {.uses = kKllUses}),
      Define("KLL_QUANTILES.MERGE_POINT_INT64", {KllMerge<true, true>()}, {.uses = kKllUses}),
      Define("KLL_QUANTILES.MERGE_POINT_FLOAT64", {KllMerge<false, true>()}, {.uses = kKllUses}),

      Define("HLL_COUNT.INIT",
             {
                 HllCountInit(googlesql::TYPE_BIGNUMERIC),
                 HllCountInit(googlesql::TYPE_UNKNOWN),
             },
             {
                 .uses = {TypeUse::kBigNumeric, TypeUse::kRejectsOver},
             }),

      Define("HLL_COUNT.MERGE_PARTIAL",
             {
                 {.function = "bq_hll_count_merge_partial"},
             },
             {
                 .uses = {TypeUse::kRejectsOver},
             }),

      Define("HLL_COUNT.MERGE",
             {
                 {.function = "bq_hll_count_merge_partial", .finish = ExtractSketch},
             },
             {
                 .uses = {TypeUse::kRejectsOver},
             }),
      // Other types count exactly, which is within any approximation error.

      Define("APPROX_COUNT_DISTINCT",
             {
                 ApproxCountDistinct(googlesql::TYPE_INT64),
                 ApproxCountDistinct(googlesql::TYPE_STRING),
                 ApproxCountDistinct(googlesql::TYPE_BYTES),
                 ApproxCountDistinct(googlesql::TYPE_NUMERIC),
                 ApproxCountDistinct(googlesql::TYPE_BIGNUMERIC),
                 {.function = "count", .distinct = AggregateRule::Distinct::kAlways},
             },
             {
                 .uses = {TypeUse::kBigNumeric, TypeUse::kRejectsOver},
                 .type_rejections = kApproxCountRejections,
             }),
      // APPROX_QUANTILES(x, n) takes the n + 1 quantiles 0, 1/n, ..., 1. quantile_disc()
      // skips NULLs, as APPROX_QUANTILES does by default.

      Define("APPROX_QUANTILES",
             {
                 {
                     .function = "quantile_disc",
                     .arguments = kQuantileArguments,
                     .nulls = kIgnore,
                 },
             },
             {
                 .uses = {TypeUse::kBigNumeric},
                 .type_rejections = kApproxQuantileRejections,
             }),
      // histogram() takes only the values; TopCount() applies the count afterwards.

      Define("APPROX_TOP_COUNT",
             {
                 {
                     .function = "histogram",
                     .arguments = kValueArgument,
                     .distinct = AggregateRule::Distinct::kUnsupported,
                     .finish = TopCount,
                 },
             },
             {
                 .uses = {TypeUse::kBigNumeric},
                 .type_rejections = kTopCountRejections,
             }),

      Define("APPROX_TOP_SUM",
             {
                 TopSumRule<googlesql::TYPE_INT64>(),
                 TopSumRule<googlesql::TYPE_NUMERIC>(),
                 TopSumRule<googlesql::TYPE_BIGNUMERIC>(),
                 TopSumRule<googlesql::TYPE_DOUBLE>(),
             },
             {
                 .uses = {TypeUse::kBigNumeric},
                 .type_rejections = kTopSumRejections,
             }),
      // The _null variants return a NULL x instead of skipping its row. INTERVAL values of y
      // compare by their keys.

      Define("MAX_BY",
             {
                 {
                     .function = "arg_max_null",
                     .conditions = ArgumentType<googlesql::TYPE_INTERVAL>::kSecond,
                     .arguments = kIntervalSecondKey,
                     .carries = true,
                 },
                 {.function = "arg_max_null", .carries = true},
             },
             {
                 .uses = {TypeUse::kAggregatesRange, TypeUse::kBigNumeric},
             }),

      Define("MIN_BY",
             {
                 {
                     .function = "arg_min_null",
                     .conditions = ArgumentType<googlesql::TYPE_INTERVAL>::kSecond,
                     .arguments = kIntervalSecondKey,
                     .carries = true,
                 },
                 {.function = "arg_min_null", .carries = true},
             },
             {
                 .uses = {TypeUse::kAggregatesRange, TypeUse::kBigNumeric},
             }),
      // DuckDB's string_agg() only joins strings, so STRING_AGG over BYTES joins their
      // hexadecimal digits; the default delimiter is b','.

      Define("STRING_AGG",
             {
                 {.function = "string_agg", .type = googlesql::TYPE_STRING, .limit = kJoin},
                 {
                     .function = "string_agg",
                     .type = googlesql::TYPE_BYTES,
                     .arguments = kHexArguments,
                     .defaults = kHexDefaults,
                     .limit = kJoin,
                     .finish = Unhex,
                 },
             }),
      // COUNT counts only TRUE after converting FALSE and NULL to NULL, and returns 0
      // for empty inputs and window frames. DISTINCT still counts TRUE at most once.

      Define("COUNTIF",
             {
                 {.function = "count", .arguments = kCountIfArguments},
             }),
      Define("LOGICAL_AND", {{.function = "bool_and"}}),
      Define("LOGICAL_OR", {{.function = "bool_or"}}),
      Define("BIT_AND", {{.function = "bit_and"}}),
      Define("BIT_OR", {{.function = "bit_or"}}),
      Define("BIT_XOR", {{.function = "bit_xor"}}),

      Define("STDDEV", kStddevSamp,
             {
                 .uses = {TypeUse::kBigNumeric},
             }),

      Define("STDDEV_SAMP", kStddevSamp,
             {
                 .uses = {TypeUse::kBigNumeric},
             }),

      Define("STDDEV_POP",
             {
                 BigNumericAggregate<kBigNumericStdDevPop>(1),
                 {.function = "stddev_pop"},
             },
             {
                 .uses = {TypeUse::kBigNumeric},
             }),

      Define("VARIANCE", kVarSamp,
             {
                 .uses = {TypeUse::kBigNumeric},
             }),

      Define("VAR_SAMP", kVarSamp,
             {
                 .uses = {TypeUse::kBigNumeric},
             }),

      Define("VAR_POP",
             {
                 BigNumericAggregate<kBigNumericVarPop>(1),
                 {.function = "var_pop"},
             },
             {
                 .uses = {TypeUse::kBigNumeric},
             }),

      Define("CORR",
             {
                 BigNumericAggregate<kBigNumericCorr>(2),
                 {.function = "corr"},
             },
             {
                 .uses = {TypeUse::kBigNumeric},
             }),

      Define("COVAR_POP",
             {
                 BigNumericAggregate<kBigNumericCovarPop>(2),
                 {.function = "covar_pop"},
             },
             {
                 .uses = {TypeUse::kBigNumeric},
             }),

      Define("COVAR_SAMP",
             {
                 BigNumericAggregate<kBigNumericCovarSamp>(2),
                 {.function = "covar_samp"},
             },
             {
                 .uses = {TypeUse::kBigNumeric},
             }),
  };
  static constexpr auto kAggregates = DefinitionViews(kAggregatesBacking);
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

// PERCENTILE_DISC of values of `type`, for a percentile of `kPercentile`.
template <googlesql::TypeKind kPercentile>
constexpr StaticAggregateRule PercentileDiscRule(googlesql::TypeKind type) {
  return {
      .function = "list",
      .type = type,
      .conditions = ArgumentType<kPercentile>::kSecond,
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
  static constexpr std::string_view kOffsetDefaults[] = {"", "1", "NULL"};
  static constexpr std::string_view kLagArguments[] = {
      "$1",
      "bq_checked_integer($2, 0, 'The offset to the function LAG must not be null', "
      "'The offset to the function LAG must not be negative')",
      "$3",
  };
  static constexpr std::string_view kLeadArguments[] = {
      "$1",
      "bq_checked_integer($2, 0, 'The offset to the function LEAD must not be null', "
      "'The offset to the function LEAD must not be negative')",
      "$3",
  };
  static constexpr std::string_view kNthValueArguments[] = {
      "$1",
      "bq_checked_integer($2, 1, 'The N value for the NthValue function must not be NULL', "
      "'The N value for the NthValue function must be positive')",
  };
  static constexpr std::string_view kNtileArguments[] = {
      "bq_checked_integer($1, 1, "
      "'The N value (number of buckets) for the NTILE function must not be NULL', "
      "'The N value (number of buckets) for the NTILE function must be positive')",
  };
  static constexpr std::string_view kTfIdfArguments[] = {"$1", "$2", "$3"};
  static constexpr std::string_view kTfIdfDefaults[] = {"", "32000", "5"};
  static constexpr auto kAnalyticsBacking = std::tuple{
      Define("ROW_NUMBER", {{.function = "row_number"}}),
      Define("RANK", {{.function = "rank"}}),
      Define("DENSE_RANK", {{.function = "dense_rank"}}),
      Define("PERCENT_RANK", {{.function = "percent_rank"}}),
      Define("CUME_DIST", {{.function = "cume_dist"}}),
      Define("NTILE", {{.function = "ntile", .arguments = kNtileArguments}}),

      Define("LAG",
             {
                 {
                     .function = "lag",
                     .arguments = kLagArguments,
                     .defaults = kOffsetDefaults,
                     .carries = true,
                     .constant_arguments = kLagConstants,
                 },
             },
             {
                 .uses = {TypeUse::kAggregatesRange, TypeUse::kBigNumeric},
             }),

      Define("LEAD",
             {
                 {
                     .function = "lead",
                     .arguments = kLeadArguments,
                     .defaults = kOffsetDefaults,
                     .carries = true,
                     .constant_arguments = kLeadConstants,
                 },
             },
             {
                 .uses = {TypeUse::kAggregatesRange, TypeUse::kBigNumeric},
             }),

      Define("FIRST_VALUE",
             {
                 {.function = "first_value", .nulls = kModifier, .carries = true},
             },
             {
                 .uses = {TypeUse::kAggregatesRange, TypeUse::kBigNumeric},
             }),

      Define("LAST_VALUE",
             {
                 {.function = "last_value", .nulls = kModifier, .carries = true},
             },
             {
                 .uses = {TypeUse::kAggregatesRange, TypeUse::kBigNumeric},
             }),

      Define("NTH_VALUE",
             {
                 {
                     .function = "nth_value",
                     .arguments = kNthValueArguments,
                     .nulls = kModifier,
                     .carries = true,
                 },
             },
             {
                 .uses = {TypeUse::kAggregatesRange, TypeUse::kBigNumeric},
             }),

      Define("PERCENTILE_CONT",
             {
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
             },
             {
                 .uses = {TypeUse::kBigNumeric},
             }),

      Define("TF_IDF",
             {
                 {
                     .function = "bq_tf_idf_dictionary",
                     .arguments = kTfIdfArguments,
                     .defaults = kTfIdfDefaults,
                     .finish = TfIdf,
                 },
             }),

      Define("PERCENTILE_DISC",
             {
                 PercentileDiscRule<googlesql::TYPE_DOUBLE>(googlesql::TYPE_DOUBLE),
                 PercentileDiscRule<googlesql::TYPE_NUMERIC>(googlesql::TYPE_DOUBLE),
                 PercentileDiscRule<googlesql::TYPE_BIGNUMERIC>(googlesql::TYPE_DOUBLE),
                 PercentileDiscRule<googlesql::TYPE_DOUBLE>(googlesql::TYPE_UNKNOWN),
                 PercentileDiscRule<googlesql::TYPE_NUMERIC>(googlesql::TYPE_UNKNOWN),
                 PercentileDiscRule<googlesql::TYPE_BIGNUMERIC>(googlesql::TYPE_UNKNOWN),
             },
             {
                 .uses = {TypeUse::kBigNumeric},
             }),
  };
  static constexpr auto kAnalytics = DefinitionViews(kAnalyticsBacking);
  return kAnalytics;
}

}  // namespace bigquery_emulator_duckdb::translator
