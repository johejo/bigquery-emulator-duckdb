#include "src/translator/functions_template.h"

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "src/duckdb_sql.h"
#include "src/translator/function_rule_builders.h"
#include "src/translator/functions.h"
#include "src/type_mapping.h"

namespace bigquery_emulator_duckdb::translator {
namespace {

using enum googlesql::TypeKind;

Condition Mode(std::size_t argument, std::string_view rounding_mode) {
  return {.argument = argument, .rounding_mode = rounding_mode};
}

Condition SubDay(std::size_t argument) {
  return Part(argument, {"hour", "minute", "second", "millisecond", "microsecond"});
}

// DuckDB rounds DECIMAL to scale zero; converting that result back to NUMERIC
// can overflow. Check the input first so SAFE. also handles that error.
std::vector<Rule> IntegralRound(std::string_view function) {
  const bool ceiling = function == "ceil";
  const std::string name(function);
  const std::string bound =
      ceiling ? "99999999999999999999999999999" : "-99999999999999999999999999999";
  return {
      BigNumericOperator("bq_bignumeric_" + name, 1),
      {
          .arity = 1,
          .spelling = "CASE WHEN $1 " + std::string(ceiling ? ">" : "<") + " CAST('" + bound +
                      "' AS DECIMAL(38, 9)) THEN !1 ELSE " + name + "($1) END",
          .conditions = {Is(1, {TYPE_NUMERIC})},
          .defaults = {},
          .errors =
              {
                  "'numeric overflow: " + ToUpperAscii(name) + "(' || bq_format('%t', $1) || ')'",
              },
      },
      {.arity = 1, .spelling = name + "($1)"},
  };
}

// Shift the requested weekday to Monday, DuckDB's week boundary, then shift back.
const std::vector<std::pair<std::string_view, int>>& WeekParts() {
  static const std::vector<std::pair<std::string_view, int>> parts = {
      {"week", 1},           {"isoweek", 0},       {"week_monday", 0}, {"week_tuesday", 6},
      {"week_wednesday", 5}, {"week_thursday", 4}, {"week_friday", 3}, {"week_saturday", 2},
  };
  return parts;
}

std::string WeekStart(const std::string& value, int shift) {
  if (shift == 0) {
    return "date_trunc('week', " + value + ")";
  }
  const std::string interval = "INTERVAL " + std::to_string(shift) + " DAY";
  return "(date_trunc('week', " + value + " + " + interval + ") - " + interval + ")";
}

// *_TRUNC to a week, with the result cast to `target` when date_trunc() would widen it.
std::vector<Rule> WeekTrunc(std::string_view target = "") {
  std::vector<Rule> rules;
  for (const auto& [part, shift] : WeekParts()) {
    const std::string start = WeekStart("$1", shift);
    rules.push_back({
        .arity = 2,
        .spelling =
            "CASE WHEN " + start + " < DATE '0001-01-01' THEN !1 ELSE " +
            (target.empty() ? start : "CAST(" + start + " AS " + std::string(target) + ")") +
            " END",
        .conditions = target == "DATE" ? std::vector<Condition>{Is(1, {TYPE_DATE}), Part(2, {part})}
                                       : std::vector<Condition>{Part(2, {part})},
        .defaults = {},
        .errors = {"'Truncating date or time to week produces an out-of-range result'"},
    });
  }
  return rules;
}

// *_TRUNC of a TIMESTAMP in a time zone. DuckDB's timezone() takes the later of a repeated civil
// time, where BigQuery takes the earlier.
std::vector<Rule> ZonedTrunc() {
  return {
      {
          .arity = 3,
          .spelling = "bq_timestamp_trunc($1, #2, $3)",
          .conditions = {Is(1, {TYPE_TIMESTAMP})},
      },
  };
}

// BigQuery counts the week boundaries crossed, DuckDB whole seven day periods.
std::vector<Rule> WeekDiff() {
  std::vector<Rule> rules;
  for (const auto& [part, shift] : WeekParts()) {
    rules.push_back({
        .arity = 3,
        .spelling = "(date_diff('day', " + WeekStart("$2", shift) + ", " + WeekStart("$1", shift) +
                    ") // 7)",
        .conditions = {Is(1, {TYPE_DATE, TYPE_DATETIME}), Part(3, {part})},
    });
  }
  return rules;
}

// LAST_DAY always returns DATE, even for a DATETIME input. Compute the first day of
// the next period before subtracting a day so the final year remains representable.
std::vector<Rule> LastDay() {
  std::vector<Rule> rules = {
      {.arity = {1, 2}, .spelling = "last_day($1)", .conditions = {Part(2, {"month"})}},
      {
          .arity = 2,
          .spelling = "CAST(date_trunc('quarter', $1) + INTERVAL 3 MONTH - INTERVAL 1 DAY AS DATE)",
          .conditions = {Part(2, {"quarter"})},
      },
      {
          .arity = 2,
          .spelling = "CAST(date_trunc('year', $1) + INTERVAL 1 YEAR - INTERVAL 1 DAY AS DATE)",
          .conditions = {Part(2, {"year"})},
      },
  };
  const auto append = [&rules](const std::string& result, Condition condition) {
    rules.push_back({
        .arity = 2,
        .spelling = "CASE WHEN " + result + " > DATE '9999-12-31' THEN !1 ELSE " + result + " END",
        .conditions = {std::move(condition)},
        .defaults = {},
        .errors = {"'LAST_DAY produces an out-of-range date'"},
    });
  };
  for (const auto& [part, shift] : WeekParts()) {
    append("CAST(" + WeekStart("$1", shift) + " + INTERVAL 6 DAY AS DATE)", Part(2, {part}));
  }
  // January 4 always belongs to its calendar year's ISO year. Truncating January 4
  // of the next ISO year gives the following year's first Monday.
  append(
      "CAST(date_trunc('week', make_date(CAST(date_part('isoyear', $1) + 1 AS INTEGER), 1, 4)) "
      "- INTERVAL 1 DAY AS DATE)",
      Part(2, {"isoyear"}));
  return rules;
}

// A cast to a civil type, where a time zone argument moves a TIMESTAMP to the civil time there.
std::vector<Rule> Civil(const std::string& target) {
  // DuckDB cannot cast a TIMESTAMPTZ to TIME, so it goes through the civil time in UTC, the
  // session's time zone.
  return {
      {
          .arity = 1,
          .spelling = "CAST(CAST($1 AS TIMESTAMP) AS " + target + ")",
          .conditions = {Is(1, {TYPE_TIMESTAMP})},
      },
      {.arity = 1, .spelling = "CAST($1 AS " + target + ")"},
      {
          .arity = 2,
          .spelling = "CAST(timezone($2, $1) AS " + target + ")",
          .conditions = {Is(1, {TYPE_TIMESTAMP})},
      },
  };
}

// RANGE_BUCKET counts boundaries <= the point, including duplicates. GoogleSQL implements it
// only in the reference evaluator; DuckDB comparisons suffice for these scalar types once NULL,
// NaN and descending boundaries have been rejected. INTERVAL arguments arrive as length keys.
// Templates bind both arguments once.
std::vector<Rule> RangeBucket() {
  std::vector<Rule> rules;
  for (const bool floating : {true, false}) {
    const std::string nan_point = floating ? " OR isnan($1)" : "";
    const std::string nan_elements =
        floating ? " WHEN len(list_filter($2, _e -> isnan(_e))) > 0 THEN !2" : "";
    std::string spelling = "CASE WHEN $1 IS NULL OR $2 IS NULL";
    spelling += nan_point;
    spelling += " THEN NULL WHEN list_count($2) <> len($2) THEN !1";
    spelling += nan_elements;
    spelling +=
        " WHEN len(list_filter($2, (_e, _i) -> _e < $2[_i - 1])) > 0 THEN !3 "
        "ELSE len(list_filter($2, _e -> _e <= $1)) END";
    rules.push_back({
        .arity = 2,
        .spelling = std::move(spelling),
        .conditions =
            {
                floating ? Is(1, {TYPE_DOUBLE})
                         : Is(1,
                              {
                                  TYPE_INT64,
                                  TYPE_NUMERIC,
                                  TYPE_BIGNUMERIC,
                                  TYPE_BOOL,
                                  TYPE_STRING,
                                  TYPE_BYTES,
                                  TYPE_DATE,
                                  TYPE_TIME,
                                  TYPE_DATETIME,
                                  TYPE_TIMESTAMP,
                                  TYPE_INTERVAL,
                              }),
            },
        .defaults = {},
        .errors =
            {
                "'Elements in input array to RANGE_BUCKET cannot be null.'",
                "'Elements in input array to RANGE_BUCKET cannot be NaN.'",
                "'Elements in input array to RANGE_BUCKET must be in ascending order.'",
            },
    });
  }
  return rules;
}

// A bound of the RANGE `range`, kRangeStart or kRangeEnd, which is infinite where unbounded.
std::string RangeBound(std::string_view range, std::string_view bound) {
  return "struct_extract(" + std::string(range) + ", " + QuoteLiteral(bound) + ")";
}

// RANGE_START and RANGE_END, which are NULL for an unbounded end.
std::vector<Rule> BoundOrNull(std::string_view bound) {
  const std::string value = RangeBound("$1", bound);
  return {{.arity = 1, .spelling = "CASE WHEN isfinite(" + value + ") THEN " + value + " END"}};
}

// A NULL bound is unbounded, and a RANGE whose start does not precede its end an error, with
// BigQuery's message.
std::vector<Rule> RangeConstructor() {
  return {
      {
          .arity = 2,
          .spelling = DuckDbRange("$1", "$2", "!1"),
          .conditions = {},
          .defaults = {},
          .errors = {QuoteLiteral(std::string(kRangeOrderError) + "; error in RANGE expression")},
      },
  };
}

// The RANGE functions compare the bounds, which DuckDB orders as BigQuery orders RANGE's: an
// unbounded start before every value and an unbounded end after every value. A NULL argument
// makes each comparison NULL, and so the result.
std::string Overlap() {
  return "(" + RangeBound("$1", kRangeStart) + " < " + RangeBound("$2", kRangeEnd) + " AND " +
         RangeBound("$2", kRangeStart) + " < " + RangeBound("$1", kRangeEnd) + ")";
}

std::vector<Rule> RangeContains() {
  return {
      {
          .arity = 2,
          .spelling = "(" + RangeBound("$1", kRangeStart) + " <= " + RangeBound("$2", kRangeStart) +
                      " AND " + RangeBound("$2", kRangeEnd) + " <= " + RangeBound("$1", kRangeEnd) +
                      ")",
          .conditions = {Is(2, {TYPE_RANGE})},
      },
      {
          .arity = 2,
          .spelling = "(" + RangeBound("$1", kRangeStart) + " <= $2 AND $2 < " +
                      RangeBound("$1", kRangeEnd) + ")",
      },
  };
}

// BigQuery's message, which unlike GoogleSQL's leaves out the ranges.
std::vector<Rule> RangeIntersect() {
  return {
      {
          .arity = 2,
          .spelling = "CASE WHEN $1 IS NULL OR $2 IS NULL THEN NULL WHEN NOT " + Overlap() +
                      " THEN !1 ELSE struct_pack(" + QuoteIdentifier(kRangeStart) +
                      " := greatest(" + RangeBound("$1", kRangeStart) + ", " +
                      RangeBound("$2", kRangeStart) + "), " + QuoteIdentifier(kRangeEnd) +
                      " := least(" + RangeBound("$1", kRangeEnd) + ", " +
                      RangeBound("$2", kRangeEnd) + ")) END",
          .conditions = {},
          .defaults = {},
          .errors =
              {
                  "'Provided RANGE inputs do not overlap. Please check RANGE_OVERLAPS before "
                  "calling "
                  "RANGE_INTERSECT; error in RANGE_INTERSECT expression'",
              },
      },
  };
}

// ROUND with ROUND_HALF_EVEN. A value is at a tie when it is as far from its truncation as from
// its rounding away from zero, and the two then differ by one unit of the last digit.
std::string RoundHalfEven() {
  const std::string away = "round($1, CAST($2 AS INTEGER))";
  const std::string toward = "trunc($1, CAST($2 AS INTEGER))";
  return "CASE WHEN " + away + " = " + toward + " OR " + away + " - $1 <> $1 - " + toward +
         " THEN " + away + " WHEN " + toward + " % (2 * (" + away + " - " + toward +
         ")) = 0 THEN " + toward + " ELSE " + away + " END";
}

// The values of a type the date arithmetic functions return, as DuckDB literals, and how a
// result outside them fails.
struct ResultRange {
  std::string_view min;
  std::string_view max;
  std::string_view error;
};

constexpr ResultRange kDateRange = {
    .min = "DATE '0001-01-01'",
    .max = "DATE '9999-12-31'",
    .error = "'Date arithmetic produces an out-of-range date'",
};
constexpr ResultRange kDatetimeRange = {
    .min = "TIMESTAMP '0001-01-01 00:00:00'",
    .max = "TIMESTAMP '9999-12-31 23:59:59.999999'",
    .error = "'Datetime arithmetic produces an out-of-range datetime'",
};
constexpr ResultRange kTimestampRange = {
    .min = "TIMESTAMPTZ '0001-01-01 00:00:00+00'",
    .max = "TIMESTAMPTZ '9999-12-31 23:59:59.999999+00'",
    .error = "'Timestamp arithmetic produces an out-of-range timestamp'",
};

// INTERVAL n PART resolves to two arguments, the count and the date part. The three argument
// form of the date arithmetic functions is the two argument `spelling` of an INTERVAL value with
// $2 multiplied out to that part. A result outside `range`, which DuckDB's wider types can hold,
// fails as in BigQuery. Each rule applies only under `conditions`.
std::vector<Rule> WithInterval(const std::string& spelling,
                               const std::optional<ResultRange>& range = std::nullopt,
                               const std::vector<Condition>& conditions = {}) {
  const auto rule = [&range, &conditions](Arity arity, const std::string& result,
                                          std::vector<Condition> part) {
    Rule rule{.arity = arity, .spelling = result, .conditions = conditions};
    rule.conditions.insert(rule.conditions.end(), part.begin(), part.end());
    if (range.has_value()) {
      rule.spelling = "CASE WHEN " + result + " NOT BETWEEN " + std::string(range->min) + " AND " +
                      std::string(range->max) + " THEN !1 ELSE " + result + " END";
      rule.errors = {std::string(range->error)};
    }
    return rule;
  };
  std::vector<Rule> rules = {rule(2, spelling, {})};
  for (const std::string_view part : {
           "year",
           "quarter",
           "month",
           "week",
           "day",
           "hour",
           "minute",
           "second",
           "millisecond",
           "microsecond",
       }) {
    std::string expanded;
    for (std::size_t i = 0; i < spelling.size(); ++i) {
      if (spelling.compare(i, 2, "$2") == 0) {
        expanded += "($2 * INTERVAL '1 " + std::string(part) + "')";
        ++i;
      } else {
        expanded += spelling.at(i);
      }
    }
    rules.push_back(rule(3, expanded, {Part(3, {part})}));
  }
  return rules;
}

// TIME arithmetic wraps within a day. Reduce the count before constructing an interval so
// every INT64 count fits DuckDB's interval representation, including INT64_MIN for subtraction.
std::vector<Rule> TimeArithmetic(std::string_view operation) {
  std::vector<Rule> rules;
  for (const auto& [part, per_day] : {
           std::pair<std::string_view, int64_t>{"hour", 24},
           {"minute", 1440},
           {"second", 86400},
           {"millisecond", 86400000},
           {"microsecond", 86400000000},
       }) {
    rules.push_back({
        .arity = 3,
        .spelling = "($1 " + std::string(operation) + " to_microseconds(($2 % " +
                    std::to_string(per_day) + ") * " + std::to_string(86400000000 / per_day) + "))",
        .conditions = {Part(3, {part})},
    });
  }
  return rules;
}

// EXTRACT, where a time zone argument moves a TIMESTAMP to the civil time there.
std::vector<Rule> Extract() {
  std::vector<Rule> rules = {
      {
          .arity = 2,
          .spelling = "date_part(#2, $1)",
          .conditions =
              {
                  Is(1, {TYPE_INTERVAL}),
                  Part(2, {"year", "month", "day", "hour", "minute", "second"}),
              },
      },
      {
          .arity = 2,
          .spelling = "(date_part(#2, $1) % 1000)",
          .conditions = {Is(1, {TYPE_INTERVAL}), Part(2, {"millisecond"})},
      },
      {
          .arity = 2,
          .spelling = "(date_part(#2, $1) % 1000000)",
          .conditions = {Is(1, {TYPE_INTERVAL}), Part(2, {"microsecond"})},
      },
  };
  for (const auto& [arity, value] :
       {std::pair<std::size_t, std::string>{2, "$1"}, {3, "timezone($3, $1)"}}) {
    rules.push_back({
        .arity = arity,
        .spelling = "(date_part('dayofweek', " + value + ") + 1)",
        .conditions =
            {
                Is(1, {TYPE_DATE, TYPE_TIME, TYPE_DATETIME, TYPE_TIMESTAMP}),
                Part(2, {"dayofweek"}),
            },
    });
    rules.push_back({
        .arity = arity,
        .spelling = "CAST(strftime(" + value + ", '%U') AS BIGINT)",
        .conditions =
            {
                Is(1, {TYPE_DATE, TYPE_TIME, TYPE_DATETIME, TYPE_TIMESTAMP}),
                Part(2, {"week"}),
            },
    });
    rules.push_back({
        .arity = arity,
        .spelling = "date_part('week', " + value + ")",
        .conditions =
            {
                Is(1, {TYPE_DATE, TYPE_TIME, TYPE_DATETIME, TYPE_TIMESTAMP}),
                Part(2, {"isoweek"}),
            },
    });
    // DuckDB counts these from the start of the minute, BigQuery from the start of the second.
    rules.push_back({
        .arity = arity,
        .spelling = "(date_part(#2, " + value + ") % 1000)",
        .conditions =
            {
                Is(1, {TYPE_DATE, TYPE_TIME, TYPE_DATETIME, TYPE_TIMESTAMP}),
                Part(2, {"millisecond"}),
            },
    });
    rules.push_back({
        .arity = arity,
        .spelling = "(date_part(#2, " + value + ") % 1000000)",
        .conditions =
            {
                Is(1, {TYPE_DATE, TYPE_TIME, TYPE_DATETIME, TYPE_TIMESTAMP}),
                Part(2, {"microsecond"}),
            },
    });
    rules.push_back({
        .arity = arity,
        .spelling = "date_part(#2, " + value + ")",
        .conditions =
            {
                Is(1, {TYPE_DATE, TYPE_TIME, TYPE_DATETIME, TYPE_TIMESTAMP}),
                Part(2,
                     {
                         "year",
                         "quarter",
                         "month",
                         "day",
                         "hour",
                         "minute",
                         "second",
                         "isoyear",
                         "dayofyear",
                     }),
            },
    });
  }
  return rules;
}

// Steps of fixed microseconds, so the session time zone plays no part. DuckDB would return an
// empty array for a zero step.
std::vector<Rule> GenerateTimestampArray() {
  std::vector<Rule> rules;
  for (const auto& [part, micros] : std::initializer_list<std::pair<std::string_view, std::string>>{
           {"day", "86400000000"},
           {"hour", "3600000000"},
           {"minute", "60000000"},
           {"second", "1000000"},
           {"millisecond", "1000"},
           {"microsecond", "1"},
       }) {
    rules.push_back({
        .arity = 4,
        .spelling = "list_transform([struct_pack(a := $1, b := $2, s := $3 * " + micros +
                    ")], _g -> CASE WHEN _g.s = 0 THEN !1 "
                    "ELSE generate_series(_g.a, _g.b, to_microseconds(_g.s)) END)[1]",
        .conditions = {Part(4, {part})},
        .defaults = {},
        .errors = {"'Sequence step cannot be 0.'"},
    });
  }
  return rules;
}

// $ARRAY_AT_OFFSET and its siblings. DuckDB would return NULL for an index out of range, and
// count negative indexes from the end.
std::vector<Rule> ArrayAt(bool ordinal, bool safe) {
  const std::string out_of_range = ordinal ? "$2 < 1 OR $2 > len($1)" : "$2 < 0 OR $2 >= len($1)";
  return {
      {
          .arity = 2,
          .spelling = "CASE WHEN $1 IS NULL OR $2 IS NULL THEN NULL WHEN " + out_of_range +
                      " THEN " + (safe ? "NULL" : "!1") + " ELSE " +
                      (ordinal ? "$1[$2]" : "$1[$2 + 1]") + " END",
          .conditions = {},
          .defaults = {},
          .errors = {"'Array index ' || $2 || ' is out of bounds'"},
      },
  };
}

// DuckDB's integer shifts fail on overflow and extend the sign; BigQuery's drop the bits shifted
// out and fill with zeros, which is what shifting a 64-bit BIT string does.
std::vector<Rule> Shift(std::string_view op) {
  return {
      {
          .arity = 2,
          .spelling = "CASE WHEN $2 < 0 THEN !1 WHEN $2 >= 64 THEN 0 ELSE CAST(CAST($1 AS BIT) " +
                      std::string(op) + " CAST($2 AS INTEGER) AS BIGINT) END",
          .conditions = {Is(1, {TYPE_INT64})},
          .defaults = {},
          .errors = {"'Bit shift by a negative value'"},
      },
  };
}

// ROUND and TRUNC of a BIGNUMERIC, which take a number of digits, and with `modes` a rounding
// mode.
std::vector<Rule> BigNumericRounding(std::string_view function, bool modes) {
  const std::string name(function);
  std::vector<Rule> rules = {
      BigNumericOperator(name, 1),
      {
          .arity = 2,
          .spelling = BigNumericCall(name + "_digits", 1, 1),
          .conditions = {Is(1, {TYPE_BIGNUMERIC})},
      },
  };
  if (modes) {
    rules.push_back({
        .arity = 3,
        .spelling = BigNumericCall(name + "_digits", 1, 1),
        .conditions = {Is(1, {TYPE_BIGNUMERIC}), Mode(3, "ROUND_HALF_AWAY_FROM_ZERO")},
    });
    rules.push_back({
        .arity = 3,
        .spelling = BigNumericCall(name + "_half_even", 1, 1),
        .conditions = {Is(1, {TYPE_BIGNUMERIC}), Mode(3, "ROUND_HALF_EVEN")},
    });
  }
  return rules;
}

// A comparison `op`, with FloatCompare's NaN for FLOAT64.
std::vector<Rule> Compare(std::string_view op) {
  return {
      {.arity = 2, .spelling = FloatCompare("$1", op, "$2"), .conditions = {Is(1, {TYPE_DOUBLE})}},
      {.arity = 2, .spelling = "($1 " + std::string(op) + " $2)"},
  };
}

// TIMESTAMP_SECONDS and its siblings, whose argument counts units of scale microseconds. DuckDB
// takes instants past BigQuery's range, and fails on some only after the query returns them.
std::vector<Rule> FromUnixTime(std::string_view timestamp, std::int64_t scale) {
  constexpr std::int64_t kMin = -62135596800000000;
  constexpr std::int64_t kMax = 253402300799999999;
  return {
      {
          .arity = 1,
          .spelling = "CASE WHEN $1 NOT BETWEEN " + std::to_string(kMin / scale) + " AND " +
                      std::to_string(kMax / scale) + " THEN !1 ELSE " + std::string(timestamp) +
                      " END",
          .conditions = {},
          .defaults = {},
          .errors =
              {
                  "'Input value ' || $1 || ' cannot be converted into a TIMESTAMP, because it "
                  "would be out of the allowed range between " +
                      std::to_string(kMin) + " to " + std::to_string(kMax) + " (microseconds)'",
              },
      },
  };
}

}  // namespace

// Functions implemented by DuckDB SQL templates.
const std::unordered_map<std::string_view, std::vector<Rule>>& TemplateRules() {
  static const auto* const kRules = new std::unordered_map<std::string_view, std::vector<Rule>>{
      // Operators. Division binds its operands once, as every rule does, and stays an
      // expression that CASE and IF can short-circuit; DuckDB would return infinity on zero.
      {"$ADD", {BigNumericOperator("bq_bignumeric_add", 2), {.arity = 2, .spelling = "($1 + $2)"}}},
      {
          "$SUBTRACT",
          {BigNumericOperator("bq_bignumeric_subtract", 2), {.arity = 2, .spelling = "($1 - $2)"}},
      },
      // DuckDB multiplies DECIMAL(38, 9) values into a DECIMAL(38, 18), which overflows past 20
      // integer digits, and divides them as DOUBLE values; GoogleSQL computes the NUMERIC result,
      // rounded half away from zero.
      {
          "$MULTIPLY",
          {
              BigNumericOperator("bq_bignumeric_multiply", 2),
              {
                  .arity = 2,
                  .spelling = "bq_multiply_numeric($1, $2)",
                  .conditions = {Is(1, {TYPE_NUMERIC})},
              },
              {.arity = 2, .spelling = "($1 * $2)"},
          },
      },
      {
          "$DIVIDE",
          {
              BigNumericOperator("bq_bignumeric_divide", 2),
              {
                  .arity = 2,
                  .spelling = "bq_divide_numeric($1, $2)",
                  .conditions = {Is(1, {TYPE_NUMERIC})},
              },
              {
                  .arity = 2,
                  .spelling = "CASE WHEN $1 IS NULL OR $2 IS NULL THEN NULL WHEN $2 = 0 THEN !1 "
                              "ELSE $1 / $2 "
                              "END",
                  .conditions = {},
                  .defaults = {},
                  .errors = {"'division by zero'"},
              },
          },
      },
      {
          "$UNARY_MINUS",
          {BigNumericOperator("bq_bignumeric_negate", 1), {.arity = 1, .spelling = "(-$1)"}},
      },
      {"$EQUAL", Compare("=")},
      {"$NOT_EQUAL", Compare("<>")},
      {"$LESS", Compare("<")},
      {"$LESS_OR_EQUAL", Compare("<=")},
      {"$GREATER", Compare(">")},
      {"$GREATER_OR_EQUAL", Compare(">=")},
      {
          "$BETWEEN",
          {
              {
                  .arity = 3,
                  .spelling = "(" + FloatCompare("$1", ">=", "$2") + " AND " +
                              FloatCompare("$1", "<=", "$3") + ")",
                  .conditions = {Is(1, {TYPE_DOUBLE})},
              },
              {.arity = 3, .spelling = "($1 BETWEEN $2 AND $3)"},
          },
      },
      // A backslash escapes the character after it, and must not end the pattern. BYTES are
      // unsupported, since DuckDB's LIKE takes only VARCHAR.
      {
          "$LIKE",
          {
              {
                  .arity = 2,
                  .spelling =
                      "CASE WHEN regexp_matches($2, '(^|[^\\\\])(\\\\\\\\)*\\\\$') THEN !1 ELSE "
                      "($1 LIKE $2 ESCAPE '\\') END",
                  .conditions = {Is(1, {TYPE_STRING})},
                  .defaults = {},
                  .errors = {"'LIKE pattern ends with a backslash'"},
              },
          },
      },
      {"$IS_DISTINCT_FROM", {{.arity = 2, .spelling = "($1 IS DISTINCT FROM $2)"}}},
      {"$IS_NOT_DISTINCT_FROM", {{.arity = 2, .spelling = "($1 IS NOT DISTINCT FROM $2)"}}},
      {"$IS_NULL", {{.arity = 1, .spelling = "($1 IS NULL)"}}},
      {"$IS_TRUE", {{.arity = 1, .spelling = "($1 IS TRUE)"}}},
      {"$IS_FALSE", {{.arity = 1, .spelling = "($1 IS FALSE)"}}},
      {"$NOT", {{.arity = 1, .spelling = "(NOT $1)"}}},
      {"$BITWISE_NOT", {{.arity = 1, .spelling = "(~$1)"}}},
      {"$BITWISE_AND", {{.arity = 2, .spelling = "($1 & $2)"}}},
      {"$BITWISE_OR", {{.arity = 2, .spelling = "($1 | $2)"}}},
      {"$BITWISE_XOR", {{.arity = 2, .spelling = "xor($1, $2)"}}},
      {"$BITWISE_LEFT_SHIFT", Shift("<<")},
      {"$BITWISE_RIGHT_SHIFT", Shift(">>")},
      // IN over the unnested elements has BigQuery's NULL handling, and is FALSE for a NULL array.
      {"$IN_ARRAY", {{.arity = 2, .spelling = "($1 IN (SELECT unnest($2)))"}}},
      {"$ARRAY_AT_OFFSET", ArrayAt(false, false)},
      {"$ARRAY_AT_ORDINAL", ArrayAt(true, false)},
      {"$SAFE_ARRAY_AT_OFFSET", ArrayAt(false, true)},
      {"$SAFE_ARRAY_AT_ORDINAL", ArrayAt(true, true)},

      // A division by zero is an error in BigQuery and +Inf in DuckDB, so SAFE_DIVIDE has to
      // make the zero itself disappear.
      {
          "SAFE_DIVIDE",
          {
              BigNumericOperator("bq_bignumeric_safe_divide", 2),
              {
                  .arity = 2,
                  .spelling = "bq_safe_divide_numeric($1, $2)",
                  .conditions = {Is(1, {TYPE_NUMERIC})},
              },
              {.arity = 2, .spelling = "($1 / NULLIF($2, 0))"},
          },
      },
      {"IEEE_DIVIDE", {{.arity = 2, .spelling = "(CAST($1 AS DOUBLE) / CAST($2 AS DOUBLE))"}}},
      // DuckDB returns NULL on a zero divisor, and MOD(x, -1) of the smallest INT64 overflows
      // in DuckDB where it is 0 in BigQuery.
      {
          "MOD",
          {
              BigNumericOperator("bq_bignumeric_mod", 2),
              {
                  .arity = 2,
                  .spelling = "CASE WHEN $1 IS NULL OR $2 IS NULL THEN NULL WHEN $2 = 0 THEN !1 "
                              "WHEN $2 = -1 "
                              "THEN 0 "
                              "ELSE mod($1, $2) END",
                  .conditions = {Is(1, {TYPE_INT64})},
                  .defaults = {},
                  .errors = {"'division by zero: MOD(' || $1 || ', ' || $2 || ')'"},
              },
              {
                  .arity = 2,
                  .spelling = "CASE WHEN $1 IS NULL OR $2 IS NULL THEN NULL WHEN $2 = 0 THEN !1 "
                              "ELSE mod($1, "
                              "$2) END",
                  .conditions = {},
                  .defaults = {},
                  .errors = {"'division by zero: MOD(' || $1 || ', ' || $2 || ')'"},
              },
          },
      },
      // DuckDB's divide() of DECIMAL values returns a DOUBLE; GoogleSQL computes the exact
      // NUMERIC quotient and checks overflow.
      {
          "DIV",
          {
              BigNumericOperator("bq_bignumeric_div", 2),
              {
                  .arity = 2,
                  .spelling = "bq_div_numeric($1, $2)",
                  .conditions = {Is(1, {TYPE_NUMERIC})},
              },
              {
                  .arity = 2,
                  .spelling =
                      "CASE WHEN $1 IS NULL OR $2 IS NULL THEN NULL WHEN $2 = 0 THEN !1 ELSE "
                      "divide($1, $2) "
                      "END",
                  .conditions = {Is(1, {TYPE_INT64})},
                  .defaults = {},
                  .errors = {"'division by zero: ' || $1 || ' / ' || $2"},
              },
          },
      },
      // DuckDB's sign() is 0 for NaN.
      {
          "SIGN",
          {
              BigNumericOperator("bq_bignumeric_sign", 1),
              {
                  .arity = 1,
                  .spelling = "CASE WHEN isnan($1) THEN $1 ELSE sign($1) END",
                  .conditions = {Is(1, {TYPE_DOUBLE})},
              },
              {.arity = 1, .spelling = "sign($1)"},
          },
      },
      // DuckDB takes the digits as an INTEGER.
      // DuckDB rounds halfway values away from zero, and its round_even() goes through
      // DOUBLE, so ROUND_HALF_EVEN takes the truncated value instead at a tie whose truncated
      // value is even. Only NUMERIC and BIGNUMERIC take a rounding mode.
      {
          "ROUND",
          Concat({
              BigNumericRounding("bq_bignumeric_round", true),
              {
                  {.arity = 1, .spelling = "round($1)"},
                  {.arity = 2, .spelling = "round($1, CAST($2 AS INTEGER))"},
                  {
                      .arity = 3,
                      .spelling = "round($1, CAST($2 AS INTEGER))",
                      .conditions = {Mode(3, "ROUND_HALF_AWAY_FROM_ZERO")},
                  },
                  {
                      .arity = 3,
                      .spelling = RoundHalfEven(),
                      .conditions = {Mode(3, "ROUND_HALF_EVEN")},
                  },
              },
          }),
      },
      {
          "TRUNC",
          Concat({
              BigNumericRounding("bq_bignumeric_trunc", false),
              {
                  {.arity = 1, .spelling = "trunc($1)"},
                  {.arity = 2, .spelling = "trunc($1, CAST($2 AS INTEGER))"},
              },
          }),
      },
      {"ABS", {BigNumericOperator("bq_bignumeric_abs", 1), {.arity = 1, .spelling = "abs($1)"}}},
      {"CEIL", IntegralRound("ceil")},
      {"CEILING", IntegralRound("ceil")},
      {"FLOOR", IntegralRound("floor")},
      // DuckDB cannot cast an empty BLOB to BIT.
      {
          "BIT_COUNT",
          {
              {.arity = 1, .spelling = "bit_count($1)", .conditions = {Is(1, {TYPE_INT64})}},
              {
                  .arity = 1,
                  .spelling =
                      "CASE WHEN octet_length($1) = 0 THEN 0 ELSE bit_count(CAST($1 AS BIT)) END",
                  .conditions = {Is(1, {TYPE_BYTES})},
              },
          },
      },

      // Date and time arithmetic: DuckDB uses the operators and puts the date part first, as a
      // string rather than as a keyword.
      // DATE_ADD and DATE_SUB also take a DATETIME or a TIMESTAMP, and return its type.
      {
          "DATE_ADD",
          Concat({
              WithInterval("CAST($1 + $2 AS DATE)", kDateRange, {Is(1, {TYPE_DATE})}),
              WithInterval("($1 + $2)", kDatetimeRange, {Is(1, {TYPE_DATETIME})}),
              WithInterval("($1 + $2)", kTimestampRange, {Is(1, {TYPE_TIMESTAMP})}),
          }),
      },
      {
          "DATE_SUB",
          Concat({
              WithInterval("CAST($1 - $2 AS DATE)", kDateRange, {Is(1, {TYPE_DATE})}),
              WithInterval("($1 - $2)", kDatetimeRange, {Is(1, {TYPE_DATETIME})}),
              WithInterval("($1 - $2)", kTimestampRange, {Is(1, {TYPE_TIMESTAMP})}),
          }),
      },
      {"DATETIME_ADD", WithInterval("($1 + $2)", kDatetimeRange)},
      {"DATETIME_SUB", WithInterval("($1 - $2)", kDatetimeRange)},
      {"TIMESTAMP_ADD", WithInterval("($1 + $2)", kTimestampRange)},
      {"TIMESTAMP_SUB", WithInterval("($1 - $2)", kTimestampRange)},
      // TIME wraps around midnight.
      {"TIME_ADD", TimeArithmetic("+")},
      {"TIME_SUB", TimeArithmetic("-")},
      {"$EXTRACT", Extract()},
      {"GENERATE_TIMESTAMP_ARRAY", GenerateTimestampArray()},
      // DATE and DATETIME differences count the boundaries crossed, which is DuckDB's date_diff,
      // while TIMESTAMP differences below a day count whole units, which is its date_sub. Its
      // date_sub of TIMESTAMPTZ goes through milliseconds, so TIMESTAMPs are compared as UTC
      // civil times.
      {
          "DATE_DIFF",
          Concat({
              WeekDiff(),
              {
                  {
                      .arity = 3,
                      .spelling = "date_sub(#3, CAST($2 AS TIMESTAMP), CAST($1 AS TIMESTAMP))",
                      .conditions = {Is(1, {TYPE_TIMESTAMP}), SubDay(3)},
                  },
              },
              {{.arity = 3, .spelling = "date_diff(#3, $2, $1)"}},
          }),
      },
      {"DATETIME_DIFF", Concat({WeekDiff(), {{.arity = 3, .spelling = "date_diff(#3, $2, $1)"}}})},
      // Every TIMESTAMP and TIME difference counts whole units.
      {
          "TIMESTAMP_DIFF",
          Concat({
              WeekDiff(),
              {
                  {
                      .arity = 3,
                      .spelling = "date_sub(#3, CAST($2 AS TIMESTAMP), CAST($1 AS TIMESTAMP))",
                      .conditions = {Is(1, {TYPE_TIMESTAMP})},
                  },
                  {.arity = 3, .spelling = "date_sub(#3, $2, $1)"},
              },
          }),
      },
      {"TIME_DIFF", Concat({WeekDiff(), {{.arity = 3, .spelling = "date_sub(#3, $2, $1)"}}})},
      // date_trunc() widens a DATE to a TIMESTAMP, which DATE_TRUNC does not. DATE_TRUNC also
      // takes a DATETIME or a TIMESTAMP, and returns its type.
      {
          "DATE_TRUNC",
          Concat({
              WeekTrunc("DATE"),
              WeekTrunc(),
              {
                  {
                      .arity = 2,
                      .spelling = "CAST(date_trunc(#2, $1) AS DATE)",
                      .conditions = {Is(1, {TYPE_DATE})},
                  },
                  {.arity = 2, .spelling = "date_trunc(#2, $1)"},
              },
              ZonedTrunc(),
          }),
      },
      {
          "DATETIME_TRUNC",
          Concat({WeekTrunc(), {{.arity = 2, .spelling = "date_trunc(#2, $1)"}}, ZonedTrunc()}),
      },
      {
          "TIMESTAMP_TRUNC",
          Concat({WeekTrunc(), {{.arity = 2, .spelling = "date_trunc(#2, $1)"}}, ZonedTrunc()}),
      },
      {
          "TIME_TRUNC",
          {{.arity = 2, .spelling = "CAST(date_trunc(#2, DATE '1970-01-01' + $1) AS TIME)"}},
      },
      {"LAST_DAY", LastDay()},

      // Constructors and conversions between the civil types and TIMESTAMP.
      {"CURRENT_DATE", {{.arity = 0, .spelling = "CURRENT_DATE"}}},
      {"CURRENT_TIME", {{.arity = 0, .spelling = "CURRENT_TIME"}}},
      {"CURRENT_TIMESTAMP", {{.arity = 0, .spelling = "CURRENT_TIMESTAMP"}}},
      {"CURRENT_DATETIME", {{.arity = 0, .spelling = "CAST(CURRENT_TIMESTAMP AS TIMESTAMP)"}}},
      {"$EXTRACT_DATE", Civil("DATE")},
      {"$EXTRACT_TIME", Civil("TIME")},
      {"$EXTRACT_DATETIME", Civil("TIMESTAMP")},
      {"DATE", Concat({{{.arity = 3, .spelling = "make_date($1, $2, $3)"}}, Civil("DATE")})},
      {"TIME", Concat({{{.arity = 3, .spelling = "make_time($1, $2, $3)"}}, Civil("TIME")})},
      {
          "DATETIME",
          Concat({
              {
                  {.arity = 6, .spelling = "make_timestamp($1, $2, $3, $4, $5, $6)"},
                  {
                      .arity = 2,
                      .spelling = "($1 + $2)",
                      .conditions = {Is(1, {TYPE_DATE}), Is(2, {TYPE_TIME})},
                  },
              },
              Civil("TIMESTAMP"),
          }),
      },
      // GoogleSQL reads a STRING, which has a time zone of its own only without the argument.
      {
          "TIMESTAMP",
          {
              {
                  .arity = 1,
                  .spelling = "bq_string_to_timestamp($1, false)",
                  .conditions = {Is(1, {TYPE_STRING})},
              },
              {
                  .arity = 2,
                  .spelling = "bq_string_to_timestamp_in($1, $2)",
                  .conditions = {Is(1, {TYPE_STRING})},
              },
              {.arity = 1, .spelling = "CAST($1 AS TIMESTAMPTZ)"},
              // A civil time in the given zone.
              {
                  .arity = 2,
                  .spelling = "timezone($2, CAST($1 AS TIMESTAMP))",
                  .conditions = {Is(1, {TYPE_DATE, TYPE_DATETIME})},
              },
          },
      },

      // Epoch conversions. The DuckDB functions return a civil timestamp, which is read as UTC
      // to arrive at the instant BigQuery means.
      // Both round down, where casting epoch() rounds to the nearest second and epoch_ms()
      // truncates toward zero.
      {
          "UNIX_SECONDS",
          {{.arity = 1, .spelling = "CAST(epoch(date_trunc('second', $1)) AS BIGINT)"}},
      },
      {"UNIX_MILLIS", {{.arity = 1, .spelling = "epoch_ms(date_trunc('millisecond', $1))"}}},
      {"UNIX_DATE", {{.arity = 1, .spelling = "date_diff('day', DATE '1970-01-01', $1)"}}},
      {"TIMESTAMP_SECONDS", FromUnixTime("to_timestamp($1)", 1000000)},
      {"TIMESTAMP_MILLIS", FromUnixTime("(epoch_ms($1) AT TIME ZONE 'UTC')", 1000)},
      {"TIMESTAMP_MICROS", FromUnixTime("(make_timestamp($1) AT TIME ZONE 'UTC')", 1)},
      {
          "DATE_FROM_UNIX_DATE",
          {
              {
                  .arity = 1,
                  .spelling = "CASE WHEN $1 NOT BETWEEN -719162 AND 2932896 THEN !1 ELSE "
                              "CAST(DATE '1970-01-01' + to_days(CAST($1 AS INTEGER)) AS DATE) END",
                  .conditions = {},
                  .defaults = {},
                  .errors = {"'DATE_FROM_UNIX_DATE range is -719162 to 2932896 but saw ' || $1"},
              },
          },
      },

      // Strings. The other string functions are GoogleSQL's, at least for BYTES; see
      // BackendRules().
      {
          "LENGTH",
          {
              {.arity = 1, .spelling = "octet_length($1)", .conditions = {Is(1, {TYPE_BYTES})}},
              {.arity = 1, .spelling = "length($1)"},
          },
      },
      {
          "BYTE_LENGTH",
          {
              {.arity = 1, .spelling = "strlen($1)", .conditions = {Is(1, {TYPE_STRING})}},
              {.arity = 1, .spelling = "octet_length($1)", .conditions = {Is(1, {TYPE_BYTES})}},
          },
      },
      // Hashes are BYTES in BigQuery and hexadecimal strings in DuckDB.
      {"MD5", {{.arity = 1, .spelling = "unhex(md5($1))"}}},
      {"SHA1", {{.arity = 1, .spelling = "unhex(sha1($1))"}}},
      {"SHA256", {{.arity = 1, .spelling = "unhex(sha256($1))"}}},
      {"TO_HEX", {{.arity = 1, .spelling = "lower(hex($1))"}}},
      {"FROM_HEX", {{.arity = 1, .spelling = "unhex($1)", .conditions = {Is(1, {TYPE_STRING})}}}},
      {"TO_BASE64", {{.arity = 1, .spelling = "to_base64($1)"}}},
      {
          "FROM_BASE64",
          {{.arity = 1, .spelling = "from_base64($1)", .conditions = {Is(1, {TYPE_STRING})}}},
      },

      {
          "ERROR",
          {
              {
                  .arity = 1,
                  .spelling = "!1",
                  .conditions = {},
                  .defaults = {},
                  .errors = {"coalesce($1, 'ERROR function called with NULL value')"},
              },
          },
      },
      {"ARRAY_REVERSE", {{.arity = 1, .spelling = "list_reverse($1)"}}},
      // Each run of equal terms in the sorted document, NULL first, becomes a term and its count.
      {
          "BAG_OF_WORDS",
          {
              {
                  .arity = 1,
                  .spelling =
                      "list_transform([list_sort($1, 'ASC', 'NULLS FIRST')], _s -> "
                      "list_transform([list_filter(range(1, len(_s) + 1), _i -> _i = 1 OR "
                      "_s[_i] IS DISTINCT FROM _s[_i - 1])], _b -> list_transform(_b, (_r, _k) -> "
                      "struct_pack(term := _s[_r], \"count\" := coalesce(_b[_k + 1], len(_s) + 1) "
                      "- "
                      "_r)))[1])[1]",
              },
          },
      },
      {"RANGE_BUCKET", RangeBucket()},
      {"RANGE", RangeConstructor()},
      {"RANGE_START", BoundOrNull(kRangeStart)},
      {"RANGE_END", BoundOrNull(kRangeEnd)},
      {"RANGE_CONTAINS", RangeContains()},
      {"RANGE_OVERLAPS", {{.arity = 2, .spelling = Overlap()}}},
      {"RANGE_INTERSECT", RangeIntersect()},
      // DuckDB's array_to_string() skips NULL elements, has no NULL text, and ignores a NULL
      // delimiter.
      {
          "ARRAY_TO_STRING",
          {
              {
                  .arity = 2,
                  .spelling = "if($2 IS NULL, NULL, array_to_string($1, $2))",
                  .conditions = {Is(2, {TYPE_STRING})},
              },
              {
                  .arity = 3,
                  .spelling = "if($2 IS NULL OR $3 IS NULL, NULL, "
                              "array_to_string(list_transform($1, _e -> coalesce(_e, $3)), $2))",
                  .conditions = {Is(2, {TYPE_STRING})},
              },
              // DuckDB has no array_to_string() for BLOB, so BYTES are concatenated with ||,
              // starting from NULL when the delimiter or NULL text is NULL.
              {
                  .arity = 2,
                  .spelling = "list_reduce(list_filter($1, _e -> _e IS NOT NULL), (_a, _e, _i) -> "
                              "_a || if(_i = 1, _e, $2 || _e), if($2 IS NULL, NULL, CAST('' AS "
                              "BLOB)))",
                  .conditions = {Is(2, {TYPE_BYTES})},
              },
              {
                  .arity = 3,
                  .spelling =
                      "list_reduce(list_transform($1, _e -> coalesce(_e, $3)), (_a, _e, _i) "
                      "-> _a || if(_i = 1, _e, $2 || _e), if($2 IS NULL OR $3 IS NULL, "
                      "NULL, CAST('' AS BLOB)))",
                  .conditions = {Is(2, {TYPE_BYTES})},
              },
          },
      },
      // generate_series() has no floating point overload, and returns an empty list for a zero
      // step, which BigQuery rejects. Other types go to GoogleSQL's implementation.
      {
          "GENERATE_ARRAY",
          {
              {
                  .arity = {2, 3},
                  .spelling = "bq_generate_array_numeric($1, $2, $3)",
                  .conditions = {Is(1, {TYPE_NUMERIC})},
                  .defaults = {"1"},
              },
              {
                  .arity = {2, 3},
                  .spelling = "bq_generate_array($1, $2, $3)",
                  .conditions = {Is(1, {TYPE_DOUBLE})},
                  .defaults = {"1"},
              },
              {
                  .arity = {2, 3},
                  .spelling =
                      "list_transform(bq_bignumeric_generate_array(CAST($1 AS VARCHAR), CAST($2 AS "
                      "VARCHAR), "
                      "CAST($3 AS VARCHAR)), _e -> CAST(_e AS BIGNUM))",
                  .conditions = {Is(1, {TYPE_BIGNUMERIC})},
                  .defaults = {"CAST('100000000000000000000000000000000000000' AS BIGNUM)"},
              },
              {
                  .arity = {2, 3},
                  .spelling = "CASE WHEN $3 = 0 THEN !1 ELSE generate_series($1, $2, $3) END",
                  .conditions = {Is(1, {TYPE_INT64}), Is(2, {TYPE_INT64}), Is(3, {TYPE_INT64})},
                  .defaults = {"1"},
                  .errors = {"'Sequence step cannot be 0.'"},
              },
          },
      },
  };
  return *kRules;
}

}  // namespace bigquery_emulator_duckdb::translator
