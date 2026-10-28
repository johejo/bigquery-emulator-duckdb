#include "src/translator/functions_template.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "src/duckdb_sql.h"
#include "src/function_catalog/rule_builders.h"
#include "src/function_catalog/rules.h"
#include "src/function_catalog/static_definitions.h"
#include "src/translator/handlers.h"
#include "src/type_mapping.h"

namespace bigquery_emulator_duckdb::translator {

using function_catalog::Define;
using function_catalog::DefinitionViews;

using function_catalog::Rule;
using function_catalog::StaticCondition;
using function_catalog::StaticRule;

using function_catalog::ArgumentMode;
using function_catalog::Arity;
using function_catalog::BigNumericCall;
using function_catalog::BigNumericOperator;
using function_catalog::Concat;
using function_catalog::Condition;
using function_catalog::FloatCompare;
using function_catalog::Implementation;
using function_catalog::Is;
using function_catalog::Part;
using function_catalog::TypeUse;

namespace {

using enum googlesql::TypeKind;

constexpr std::array kIntervalTypes = {TYPE_INTERVAL};
constexpr std::array kStructTypes = {TYPE_STRUCT};
constexpr std::array kRangeTypes = {TYPE_RANGE};
constexpr std::array kIntervalDoubleTypes = {TYPE_INTERVAL, TYPE_DOUBLE};
constexpr std::array kDoubleIntervalTypes = {TYPE_DOUBLE, TYPE_INTERVAL};

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
constexpr auto kWeekParts = std::to_array<std::pair<std::string_view, int>>({
    {"week", 1},
    {"isoweek", 0},
    {"week_monday", 0},
    {"week_tuesday", 6},
    {"week_wednesday", 5},
    {"week_thursday", 4},
    {"week_friday", 3},
    {"week_saturday", 2},
});

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
  rules.reserve(kWeekParts.size());
  for (const auto& [part, shift] : kWeekParts) {
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
  rules.reserve(kWeekParts.size());
  for (const auto& [part, shift] : kWeekParts) {
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
  for (const auto& [part, shift] : kWeekParts) {
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
          // A TIME does not need the date to be in range.
          .spelling = target == "TIME"
                          ? "bq_timestamp_to_time_in($1, $2)"
                          : "CAST(bq_timestamp_to_datetime_in($1, $2) AS " + target + ")",
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
        floating ? " WHEN len(list_filter($2, lambda _e: isnan(_e))) > 0 THEN !2" : "";
    std::string spelling = "CASE WHEN $1 IS NULL OR $2 IS NULL";
    spelling += nan_point;
    spelling += " THEN NULL WHEN list_count($2) <> len($2) THEN !1";
    spelling += nan_elements;
    spelling +=
        " WHEN len(list_filter($2, lambda _e, _i: _e < $2[_i - 1])) > 0 THEN !3 "
        "ELSE len(list_filter($2, lambda _e: _e <= $1)) END";
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

// DuckDB v2 binds DECIMAL precision before evaluating NULL arguments. Give the binder a
// non-NULL precision, then propagate the original NULL as BigQuery does.
std::string RoundWithDigits(std::string_view function) {
  return "CASE WHEN $2 IS NULL THEN NULL ELSE " + std::string(function) +
         "($1, CAST(coalesce($2, 0) AS INTEGER)) END";
}

// ROUND with ROUND_HALF_EVEN. A value is at a tie when it is as far from its truncation as from
// its rounding away from zero, and the two then differ by one unit of the last digit.
std::string RoundHalfEven() {
  const std::string away = "round($1, CAST(coalesce($2, 0) AS INTEGER))";
  const std::string toward = "trunc($1, CAST(coalesce($2, 0) AS INTEGER))";
  return "CASE WHEN $2 IS NULL THEN NULL WHEN " + away + " = " + toward + " OR " + away +
         " - $1 <> $1 - " + toward + " THEN " + away + " WHEN " + toward + " % (2 * (" + away +
         " - " + toward + ")) = 0 THEN " + toward + " ELSE " + away + " END";
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
       {std::pair<std::size_t, std::string>{2, "$1"}, {3, "bq_timestamp_civil_in($1, $3)"}}) {
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
                    ")], lambda _g: CASE WHEN _g.s = 0 THEN !1 "
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

// Literal templates have immutable backing arrays; generated SQL stays in TemplateRules().
std::span<const function_catalog::StaticNamedDefinition<StaticRule>> StaticTemplateRules() {
  static constexpr googlesql::TypeKind kElementTypes[] = {TYPE_GRAPH_ELEMENT};
  static constexpr googlesql::TypeKind kPathTypes[] = {TYPE_GRAPH_PATH};
  static constexpr StaticCondition kElement[] = {{.argument = 1, .types = kElementTypes}};
  static constexpr StaticCondition kPath[] = {{.argument = 1, .types = kPathTypes}};
  static constexpr StaticCondition kEndpoints[] = {
      {.argument = 1, .types = kElementTypes},
      {.argument = 2, .types = kElementTypes},
  };
  static constexpr std::string_view kEndpointErrors[] = {
      "'The emulator does not support graph endpoint predicates with NULL operands'",
  };
  static constexpr googlesql::TypeKind kStringTypes[] = {TYPE_STRING};
  static constexpr googlesql::TypeKind kBytesTypes[] = {TYPE_BYTES};
  static constexpr googlesql::TypeKind kInt64Types[] = {TYPE_INT64};
  static constexpr googlesql::TypeKind kNumericTypes[] = {TYPE_NUMERIC};
  static constexpr googlesql::TypeKind kDoubleTypes[] = {TYPE_DOUBLE};
  static constexpr googlesql::TypeKind kBigNumericTypes[] = {TYPE_BIGNUMERIC};
  static constexpr googlesql::TypeKind kCivilTypes[] = {TYPE_DATE, TYPE_DATETIME};
  static constexpr StaticCondition kString[] = {{.argument = 1, .types = kStringTypes}};
  static constexpr StaticCondition kBytes[] = {{.argument = 1, .types = kBytesTypes}};
  static constexpr StaticCondition kInt64[] = {{.argument = 1, .types = kInt64Types}};
  static constexpr StaticCondition kNumeric[] = {{.argument = 1, .types = kNumericTypes}};
  static constexpr StaticCondition kDouble[] = {{.argument = 1, .types = kDoubleTypes}};
  static constexpr StaticCondition kBigNumeric[] = {{.argument = 1, .types = kBigNumericTypes}};
  static constexpr StaticCondition kCivil[] = {{.argument = 1, .types = kCivilTypes}};
  static constexpr StaticCondition kStringDelimiter[] = {{.argument = 2, .types = kStringTypes}};
  static constexpr StaticCondition kBytesDelimiter[] = {{.argument = 2, .types = kBytesTypes}};
  static constexpr StaticCondition kInt64Sequence[] = {
      {.argument = 1, .types = kInt64Types},
      {.argument = 2, .types = kInt64Types},
      {.argument = 3, .types = kInt64Types},
  };
  static constexpr std::string_view kLikeErrors[] = {"'LIKE pattern ends with a backslash'"};
  static constexpr function_catalog::TypeRejection kDistinctRejections[] = {
      {
          .types = kStructTypes,
          .message = "IS DISTINCT FROM can't be used with arguments of type STRUCT",
      },
  };
  static constexpr function_catalog::TypeRejection kNotDistinctRejections[] = {
      {
          .types = kStructTypes,
          .message = "IS NOT DISTINCT FROM can't be used with arguments of type STRUCT",
      },
  };
  static constexpr ArgumentMode kInArrayModes[] = {
      ArgumentMode::kExpression,
      ArgumentMode::kIntervalArrayKeys,
  };
  static constexpr std::string_view kDateFromUnixDateErrors[] = {
      "'DATE_FROM_UNIX_DATE range is -719162 to 2932896 but saw ' || $1",
  };
  static constexpr std::string_view kErrorMessages[] = {
      "coalesce($1, 'ERROR function called with NULL value')",
  };
  static constexpr std::string_view kOneDefault[] = {"1"};
  static constexpr std::string_view kBigNumericOneDefault[] = {
      "CAST('100000000000000000000000000000000000000' AS BIGNUM)",
  };
  static constexpr std::string_view kSequenceErrors[] = {"'Sequence step cannot be 0.'"};
  static constexpr googlesql::TypeKind kDateTypes[] = {TYPE_DATE};
  static constexpr googlesql::TypeKind kDatetimeTypes[] = {TYPE_DATETIME};
  static constexpr googlesql::TypeKind kTimestampTypes[] = {TYPE_TIMESTAMP};
  static constexpr googlesql::TypeKind kTimeTypes[] = {TYPE_TIME};
  static constexpr StaticCondition kInterval[] = {{.argument = 1, .types = kIntervalTypes}};
  static constexpr StaticCondition kIntervals[] = {
      {.argument = 1, .types = kIntervalTypes},
      {.argument = 2, .types = kIntervalTypes},
  };
  static constexpr StaticCondition kDateInterval[] = {
      {.argument = 1, .types = kDateTypes},
      {.argument = 2, .types = kIntervalTypes},
  };
  static constexpr StaticCondition kIntervalDate[] = {
      {.argument = 1, .types = kIntervalTypes},
      {.argument = 2, .types = kDateTypes},
  };
  static constexpr StaticCondition kDates[] = {
      {.argument = 1, .types = kDateTypes},
      {.argument = 2, .types = kDateTypes},
  };
  static constexpr StaticCondition kDatetime[] = {{.argument = 1, .types = kDatetimeTypes}};
  static constexpr StaticCondition kDatetimeSecond[] = {{.argument = 2, .types = kDatetimeTypes}};
  static constexpr StaticCondition kDatetimeInterval[] = {
      {.argument = 1, .types = kDatetimeTypes},
      {.argument = 2, .types = kIntervalTypes},
  };
  static constexpr StaticCondition kTimestamp[] = {{.argument = 1, .types = kTimestampTypes}};
  static constexpr StaticCondition kTimestampSecond[] = {{.argument = 2, .types = kTimestampTypes}};
  static constexpr StaticCondition kTimestampInterval[] = {
      {.argument = 1, .types = kTimestampTypes},
      {.argument = 2, .types = kIntervalTypes},
  };
  static constexpr StaticCondition kTime[] = {{.argument = 1, .types = kTimeTypes}};
  static constexpr StaticCondition kIntervalInt64[] = {
      {.argument = 1, .types = kIntervalTypes},
      {.argument = 2, .types = kInt64Types},
  };
  static constexpr StaticCondition kInt64Interval[] = {
      {.argument = 1, .types = kInt64Types},
      {.argument = 2, .types = kIntervalTypes},
  };
  static constexpr function_catalog::TypeRejection kMultiplyRejections[] = {
      {.types = kIntervalDoubleTypes, .message = "No matching signature for operator *"},
      {.types = kDoubleIntervalTypes, .message = "No matching signature for operator *"},
  };
  static constexpr std::string_view kDivisionErrors[] = {"'division by zero'"};
  static constexpr std::string_view kDoubleOverflowErrors[] = {"'double overflow'"};
  static constexpr std::string_view kDoubleDivisionErrors[] = {
      "'division by zero'",
      "'double overflow'",
  };
  static constexpr std::string_view kModErrors[] = {
      "'division by zero: MOD(' || $1 || ', ' || $2 || ')'",
  };
  static constexpr std::string_view kDivErrors[] = {"'division by zero: ' || $1 || ' / ' || $2"};
  static constexpr auto kRulesBacking = std::tuple{
      // A backslash escapes the character after it, and must not end the pattern. BYTES are
      // unsupported, since DuckDB's LIKE takes only VARCHAR.

      Define("$LIKE",
             {
                 {
                     .arity = 2,
                     .spelling = "CASE WHEN regexp_matches($2, '(^|[^\\\\])(\\\\\\\\)*\\\\$') "
                                 "THEN !1 ELSE "
                                 "($1 LIKE $2 ESCAPE '\\') END",
                     .conditions = kString,
                     .errors = kLikeErrors,
                 },
             }),

      Define("$IS_DISTINCT_FROM",
             {
                 {.arity = 2, .spelling = "($1 IS DISTINCT FROM $2)"},
             },
             {
                 .uses = {TypeUse::kCompares, TypeUse::kComparesIntervalKeys, TypeUse::kBigNumeric},
                 .type_rejections = kDistinctRejections,
             }),

      Define("$IS_NOT_DISTINCT_FROM",
             {
                 {.arity = 2, .spelling = "($1 IS NOT DISTINCT FROM $2)"},
             },
             {
                 .uses = {TypeUse::kCompares, TypeUse::kComparesIntervalKeys, TypeUse::kBigNumeric},
                 .type_rejections = kNotDistinctRejections,
             }),

      Define("$IS_NULL", {{.arity = 1, .spelling = "($1 IS NULL)"}},
             {
                 .uses = {TypeUse::kCarries, TypeUse::kGraphValues, TypeUse::kBigNumeric},
             }),
      // Operators stay expressions that CASE and IF can short-circuit. GoogleSQL adds an
      // INTERVAL's months, days and time in turn, checking each step's range; see
      // src/googlesql_functions_duckdb/interval.cc.
      Define(
          "$ADD",
          {
              {
                  .arity = 2,
                  .spelling =
                      "CAST(bq_bignumeric_add(CAST($1 AS VARCHAR), CAST($2 AS VARCHAR)) AS BIGNUM)",
                  .conditions = kBigNumeric,
              },
              {.arity = 2, .spelling = "bq_add_intervals($1, $2)", .conditions = kIntervals},
              {.arity = 2, .spelling = "bq_add_date_interval($1, $2)", .conditions = kDateInterval},
              {.arity = 2, .spelling = "bq_add_date_interval($2, $1)", .conditions = kIntervalDate},
              {.arity = 2, .spelling = "bq_add_datetime_interval($1, $2)", .conditions = kDatetime},
              {
                  .arity = 2,
                  .spelling = "bq_add_datetime_interval($2, $1)",
                  .conditions = kDatetimeSecond,
              },
              {
                  .arity = 2,
                  .spelling = "bq_add_timestamp_interval($1, $2)",
                  .conditions = kTimestamp,
              },
              {
                  .arity = 2,
                  .spelling = "bq_add_timestamp_interval($2, $1)",
                  .conditions = kTimestampSecond,
              },
              // An infinity from finite inputs is overflow; input infinities and NaNs
              // retain IEEE behavior. Templates bind repeated arguments once.
              {
                  .arity = 2,
                  .spelling = "CASE WHEN isfinite($1) AND isfinite($2) AND isinf($1 + $2) "
                              "THEN !1 ELSE $1 + $2 END",
                  .conditions = kDouble,
                  .errors = kDoubleOverflowErrors,
              },
              {.arity = 2, .spelling = "($1 + $2)"},
          },
          {.uses = {TypeUse::kBuildsOrReadsInterval, TypeUse::kBigNumeric}}),
      // - subtracts an INTERVAL as + adds its negation.
      Define(
          "$SUBTRACT",
          {
              {
                  .arity = 2,
                  .spelling =
                      "CAST(bq_bignumeric_subtract(CAST($1 AS VARCHAR), CAST($2 AS VARCHAR)) "
                      "AS BIGNUM)",
                  .conditions = kBigNumeric,
              },
              {.arity = 2, .spelling = "bq_subtract_intervals($1, $2)", .conditions = kInterval},
              {
                  .arity = 2,
                  .spelling = "bq_subtract_date_interval($1, $2)",
                  .conditions = kDateInterval,
              },
              {.arity = 2, .spelling = "bq_subtract_dates($1, $2)", .conditions = kDates},
              {
                  .arity = 2,
                  .spelling = "bq_subtract_datetime_interval($1, $2)",
                  .conditions = kDatetimeInterval,
              },
              {.arity = 2, .spelling = "bq_subtract_datetimes($1, $2)", .conditions = kDatetime},
              {
                  .arity = 2,
                  .spelling = "bq_subtract_timestamp_interval($1, $2)",
                  .conditions = kTimestampInterval,
              },
              {.arity = 2, .spelling = "bq_subtract_timestamps($1, $2)", .conditions = kTimestamp},
              {.arity = 2, .spelling = "bq_subtract_times($1, $2)", .conditions = kTime},
              {
                  .arity = 2,
                  .spelling = "CASE WHEN isfinite($1) AND isfinite($2) AND isinf($1 - $2) "
                              "THEN !1 ELSE $1 - $2 END",
                  .conditions = kDouble,
                  .errors = kDoubleOverflowErrors,
              },
              {.arity = 2, .spelling = "($1 - $2)"},
          },
          {.uses = {TypeUse::kBuildsOrReadsInterval, TypeUse::kBigNumeric}}),
      // DuckDB multiplies DECIMAL(38, 9) into DECIMAL(38, 18), overflowing past 20
      // integer digits, and divides as DOUBLE; GoogleSQL computes the NUMERIC result.
      Define("$MULTIPLY",
             {
                 {
                     .arity = 2,
                     .spelling = "CAST(bq_bignumeric_multiply(CAST($1 AS VARCHAR), CAST($2 AS "
                                 "VARCHAR)) AS BIGNUM)",
                     .conditions = kBigNumeric,
                 },
                 {.arity = 2, .spelling = "bq_multiply_numeric($1, $2)", .conditions = kNumeric},
                 // Reject FLOAT64 operands as BigQuery does.
                 {
                     .arity = 2,
                     .spelling = "bq_multiply_interval($1, $2)",
                     .conditions = kIntervalInt64,
                 },
                 {
                     .arity = 2,
                     .spelling = "bq_multiply_interval($2, $1)",
                     .conditions = kInt64Interval,
                 },
                 {
                     .arity = 2,
                     .spelling = "CASE WHEN isfinite($1) AND isfinite($2) AND isinf($1 * $2) "
                                 "THEN !1 ELSE $1 * $2 END",
                     .conditions = kDouble,
                     .errors = kDoubleOverflowErrors,
                 },
                 {.arity = 2, .spelling = "($1 * $2)"},
             },
             {
                 .uses = {TypeUse::kBuildsOrReadsInterval, TypeUse::kBigNumeric},
                 .type_rejections = kMultiplyRejections,
             }),
      // DuckDB would return infinity on division by zero.
      Define(
          "$DIVIDE",
          {
              {
                  .arity = 2,
                  .spelling = "CAST(bq_bignumeric_divide(CAST($1 AS VARCHAR), CAST($2 AS VARCHAR)) "
                              "AS BIGNUM)",
                  .conditions = kBigNumeric,
              },
              {.arity = 2, .spelling = "bq_divide_numeric($1, $2)", .conditions = kNumeric},
              {.arity = 2, .spelling = "bq_divide_interval($1, $2)", .conditions = kIntervalInt64},
              {
                  .arity = 2,
                  .spelling = "CASE WHEN $1 IS NULL OR $2 IS NULL THEN NULL WHEN $2 = 0 THEN !1 "
                              "WHEN isfinite($1) AND isfinite($2) AND isinf($1 / $2) THEN !2 "
                              "ELSE $1 / $2 END",
                  .conditions = kDouble,
                  .errors = kDoubleDivisionErrors,
              },
              {
                  .arity = 2,
                  .spelling =
                      "CASE WHEN $1 IS NULL OR $2 IS NULL THEN NULL WHEN $2 = 0 THEN !1 ELSE "
                      "$1 / $2 END",
                  .errors = kDivisionErrors,
              },
          },
          {.uses = {TypeUse::kBuildsOrReadsInterval, TypeUse::kBigNumeric}}),
      Define("$UNARY_MINUS",
             {
                 {
                     .arity = 1,
                     .spelling = "CAST(bq_bignumeric_negate(CAST($1 AS VARCHAR)) AS BIGNUM)",
                     .conditions = kBigNumeric,
                 },
                 {.arity = 1, .spelling = "(-$1)"},
             },
             {.uses = {TypeUse::kBuildsOrReadsInterval, TypeUse::kBigNumeric}}),
      // SAFE_DIVIDE returns NULL for both division by zero and finite-input overflow. CASE avoids
      // DuckDB v2's NULLIF macro binder.
      Define("SAFE_DIVIDE",
             {
                 {
                     .arity = 2,
                     .spelling = "CAST(bq_bignumeric_safe_divide(CAST($1 AS VARCHAR), CAST($2 AS "
                                 "VARCHAR)) AS BIGNUM)",
                     .conditions = kBigNumeric,
                 },
                 {.arity = 2, .spelling = "bq_safe_divide_numeric($1, $2)", .conditions = kNumeric},
                 {
                     .arity = 2,
                     .spelling = "CASE WHEN $2 = 0 OR (isfinite($1) AND isfinite($2) AND "
                                 "isinf($1 / $2)) THEN NULL ELSE $1 / $2 END",
                     .conditions = kDouble,
                 },
                 {.arity = 2, .spelling = "($1 / CASE WHEN $2 = 0 THEN NULL ELSE $2 END)"},
             },
             {.uses = {TypeUse::kBigNumeric}}),
      // DuckDB returns NULL on a zero divisor, and MOD(INT64_MIN, -1) overflows instead of 0.
      Define(
          "MOD",
          {
              {
                  .arity = 2,
                  .spelling =
                      "CAST(bq_bignumeric_mod(CAST($1 AS VARCHAR), CAST($2 AS VARCHAR)) AS BIGNUM)",
                  .conditions = kBigNumeric,
              },
              {
                  .arity = 2,
                  .spelling = "CASE WHEN $1 IS NULL OR $2 IS NULL THEN NULL WHEN $2 = 0 THEN !1 "
                              "WHEN $2 = -1 THEN 0 ELSE mod($1, $2) END",
                  .conditions = kInt64,
                  .errors = kModErrors,
              },
              {
                  .arity = 2,
                  .spelling = "CASE WHEN $1 IS NULL OR $2 IS NULL THEN NULL WHEN $2 = 0 THEN !1 "
                              "ELSE mod($1, $2) END",
                  .errors = kModErrors,
              },
          },
          {.uses = {TypeUse::kBigNumeric}}),
      // DuckDB's divide() of DECIMAL returns DOUBLE; GoogleSQL computes the exact quotient.
      Define(
          "DIV",
          {
              {
                  .arity = 2,
                  .spelling =
                      "CAST(bq_bignumeric_div(CAST($1 AS VARCHAR), CAST($2 AS VARCHAR)) AS BIGNUM)",
                  .conditions = kBigNumeric,
              },
              {.arity = 2, .spelling = "bq_div_numeric($1, $2)", .conditions = kNumeric},
              {
                  .arity = 2,
                  .spelling = "CASE WHEN $1 IS NULL OR $2 IS NULL THEN NULL WHEN $2 = 0 THEN !1 "
                              "ELSE divide($1, $2) END",
                  .conditions = kInt64,
                  .errors = kDivErrors,
              },
          },
          {.uses = {TypeUse::kBigNumeric}}),
      // DuckDB's sign() is 0 for NaN.
      Define("SIGN",
             {
                 {
                     .arity = 1,
                     .spelling = "CAST(bq_bignumeric_sign(CAST($1 AS VARCHAR)) AS BIGNUM)",
                     .conditions = kBigNumeric,
                 },
                 {
                     .arity = 1,
                     .spelling = "CASE WHEN isnan($1) THEN $1 ELSE sign($1) END",
                     .conditions = kDouble,
                 },
                 {.arity = 1, .spelling = "sign($1)"},
             },
             {.uses = {TypeUse::kBigNumeric}}),
      Define("ABS",
             {
                 {
                     .arity = 1,
                     .spelling = "CAST(bq_bignumeric_abs(CAST($1 AS VARCHAR)) AS BIGNUM)",
                     .conditions = kBigNumeric,
                 },
                 {.arity = 1, .spelling = "abs($1)"},
             },
             {.uses = {TypeUse::kBigNumeric}}),
      // A collation is a type annotation, which GoogleSQL propagates to the operations that use
      // it; the value is the string itself, once the specification is checked.
      Define("COLLATE", {{.arity = 2, .spelling = "bq_collate($1, $2)"}}),
      Define("$IS_TRUE", {{.arity = 1, .spelling = "($1 IS TRUE)"}}),
      Define("$IS_FALSE", {{.arity = 1, .spelling = "($1 IS FALSE)"}}),
      Define("$NOT", {{.arity = 1, .spelling = "(NOT $1)"}}),
      Define("$BITWISE_NOT", {{.arity = 1, .spelling = "(~$1)"}}),
      Define("$BITWISE_AND", {{.arity = 2, .spelling = "($1 & $2)"}}),
      Define("$BITWISE_OR", {{.arity = 2, .spelling = "($1 | $2)"}}),
      Define("$BITWISE_XOR", {{.arity = 2, .spelling = "xor($1, $2)"}}),

      // IN over the unnested elements has BigQuery's NULL handling, and is FALSE for a NULL
      // array.
      Define("$IN_ARRAY",
             {
                 {.arity = 2, .spelling = "($1 IN (SELECT unnest($2)))"},
             },
             {
                 .uses =
                     {
                         TypeUse::kCompares,
                         TypeUse::kComparesIntervalKeys,
                         TypeUse::kComparesStructsWithInterval,
                         TypeUse::kBigNumeric,
                     },
                 .argument_modes = kInArrayModes,
                 .struct_handler = StructComparison,
             }),

      Define("IEEE_DIVIDE",
             {
                 {.arity = 2, .spelling = "(CAST($1 AS DOUBLE) / CAST($2 AS DOUBLE))"},
             }),
      // DuckDB cannot cast an empty BLOB to BIT.

      Define("BIT_COUNT",
             {
                 {.arity = 1, .spelling = "bit_count($1)", .conditions = kInt64},
                 {
                     .arity = 1,
                     .spelling = "CASE WHEN octet_length($1) = 0 THEN 0 ELSE "
                                 "bit_count(CAST($1 AS BIT)) END",
                     .conditions = kBytes,
                 },
             }),

      Define("TIME_TRUNC",
             {
                 {
                     .arity = 2,
                     .spelling = "CAST(date_trunc(#2, DATE '1970-01-01' + $1) AS TIME)",
                 },
             }),
      Define("CURRENT_DATE", {{.arity = 0, .spelling = "CURRENT_DATE"}}),
      Define("CURRENT_TIME", {{.arity = 0, .spelling = "CURRENT_TIME"}}),
      Define("CURRENT_TIMESTAMP", {{.arity = 0, .spelling = "CURRENT_TIMESTAMP"}}),

      Define("CURRENT_DATETIME",
             {
                 {.arity = 0, .spelling = "CAST(CURRENT_TIMESTAMP AS TIMESTAMP)"},
             }),
      // GoogleSQL reads a STRING, which has a time zone of its own only without the argument.

      Define("TIMESTAMP",
             {
                 {
                     .arity = 1,
                     .spelling = "bq_string_to_timestamp($1, false)",
                     .conditions = kString,
                 },
                 {
                     .arity = 2,
                     .spelling = "bq_string_to_timestamp_in($1, $2)",
                     .conditions = kString,
                 },
                 {.arity = 1, .spelling = "CAST($1 AS TIMESTAMPTZ)"},
                 // A civil time in the given zone.
                 {
                     .arity = 2,
                     .spelling = "bq_datetime_to_timestamp_in(CAST($1 AS TIMESTAMP), $2)",
                     .conditions = kCivil,
                 },
             }),
      // Both round down, where casting epoch() rounds to the nearest second and epoch_ms()
      // truncates toward zero.

      Define("UNIX_SECONDS",
             {
                 {.arity = 1, .spelling = "CAST(epoch(date_trunc('second', $1)) AS BIGINT)"},
             }),

      Define("UNIX_MILLIS",
             {
                 {.arity = 1, .spelling = "epoch_ms(date_trunc('millisecond', $1))"},
             }),

      Define("UNIX_DATE",
             {
                 {.arity = 1, .spelling = "date_diff('day', DATE '1970-01-01', $1)"},
             }),

      Define(
          "DATE_FROM_UNIX_DATE",
          {
              {
                  .arity = 1,
                  .spelling = "CASE WHEN $1 NOT BETWEEN -719162 AND 2932896 THEN !1 ELSE "
                              "CAST(DATE '1970-01-01' + to_days(CAST($1 AS INTEGER)) AS DATE) END",
                  .errors = kDateFromUnixDateErrors,
              },
          }),
      // Strings. The other string functions are GoogleSQL's, at least for BYTES; see
      // BackendRules().

      Define("LENGTH",
             {
                 {.arity = 1, .spelling = "octet_length($1)", .conditions = kBytes},
                 {.arity = 1, .spelling = "length($1)"},
             }),

      Define("BYTE_LENGTH",
             {
                 {.arity = 1, .spelling = "strlen($1)", .conditions = kString},
                 {.arity = 1, .spelling = "octet_length($1)", .conditions = kBytes},
             }),
      // Hashes are BYTES in BigQuery and hexadecimal strings in DuckDB.
      Define("MD5", {{.arity = 1, .spelling = "unhex(md5($1))"}}),
      Define("SHA1", {{.arity = 1, .spelling = "unhex(sha1($1))"}}),
      Define("SHA256", {{.arity = 1, .spelling = "unhex(sha256($1))"}}),
      Define("TO_HEX", {{.arity = 1, .spelling = "lower(hex($1))"}}),

      Define("FROM_HEX",
             {
                 {.arity = 1, .spelling = "unhex($1)", .conditions = kString},
             }),
      Define("TO_BASE64", {{.arity = 1, .spelling = "to_base64($1)"}}),

      Define("FROM_BASE64",
             {
                 {.arity = 1, .spelling = "from_base64($1)", .conditions = kString},
             }),

      Define("ERROR",
             {
                 {.arity = 1, .spelling = "!1", .errors = kErrorMessages},
             },
             {
                 .uses =
                     {
                         TypeUse::kBuildsOrReadsInterval,
                         TypeUse::kBuildsOrReadsRange,
                         TypeUse::kRejectsSafe,
                         TypeUse::kBigNumeric,
                     },
             }),

      Define("ARRAY_REVERSE", {{.arity = 1, .spelling = "list_reverse($1)"}},
             {
                 .uses = {TypeUse::kCarries, TypeUse::kBigNumeric},
             }),
      // Each run of equal terms in the sorted document, NULL first, becomes a term and its count.

      Define(
          "BAG_OF_WORDS",
          {
              {
                  .arity = 1,
                  .spelling = "list_transform([list_sort($1, 'ASC', 'NULLS FIRST')], lambda _s: "
                              "list_transform([list_filter(range(1, len(_s) + 1), lambda _i: _i = "
                              "1 OR "
                              "_s[_i] IS DISTINCT FROM _s[_i - 1])], lambda _b: list_transform(_b, "
                              "lambda "
                              "_r, _k: struct_pack(term := _s[_r], \"count\" := coalesce(_b[_k + "
                              "1], "
                              "len(_s) + 1) - _r)))[1])[1]",
              },
          }),
      // Use list_aggr directly: DuckDB v2 macros can mistake aliases on expanded arguments
      // for named parameters. Preserve array_to_string's empty-array result and NULL handling.

      Define(
          "ARRAY_TO_STRING",
          {
              {
                  .arity = 2,
                  .spelling = "CASE WHEN $2 IS NULL THEN NULL WHEN len($1) = 0 THEN '' "
                              "ELSE list_aggr($1, 'string_agg', $2) END",
                  .conditions = kStringDelimiter,
              },
              {
                  .arity = 3,
                  .spelling = "CASE WHEN $2 IS NULL OR $3 IS NULL THEN NULL WHEN len($1) = 0 THEN "
                              "'' "
                              "ELSE list_aggr(list_transform($1, lambda _e: coalesce(_e, $3)), "
                              "'string_agg', $2) END",
                  .conditions = kStringDelimiter,
              },
              // DuckDB has no array_to_string() for BLOB, so BYTES are concatenated with
              // ||, starting from NULL when the delimiter or NULL text is NULL.
              {
                  .arity = 2,
                  .spelling = "list_reduce(list_filter($1, lambda _e: _e IS NOT NULL), lambda "
                              "_a, _e, _i: "
                              "_a || CASE WHEN _i = 1 THEN _e ELSE $2 || _e END, "
                              "CASE WHEN $2 IS NULL THEN NULL ELSE CAST('' AS BLOB) END)",
                  .conditions = kBytesDelimiter,
              },
              {
                  .arity = 3,
                  .spelling = "list_reduce(list_transform($1, lambda _e: coalesce(_e, "
                              "$3)), lambda _a, "
                              "_e, _i: "
                              "_a || CASE WHEN _i = 1 THEN _e ELSE $2 || _e END, "
                              "CASE WHEN $2 IS NULL OR $3 IS NULL THEN NULL ELSE CAST('' "
                              "AS BLOB) END)",
                  .conditions = kBytesDelimiter,
              },
          }),
      // generate_series() has no floating point overload, and returns an empty list for a zero
      // step, which BigQuery rejects. Other types go to GoogleSQL's implementation.

      Define("GENERATE_ARRAY",
             {
                 {
                     .arity = {2, 3},
                     .spelling = "bq_generate_array_numeric($1, $2, $3)",
                     .conditions = kNumeric,
                     .defaults = kOneDefault,
                     .implementation = Implementation::kGoogleSQL,
                 },
                 {
                     .arity = {2, 3},
                     .spelling = "bq_generate_array($1, $2, $3)",
                     .conditions = kDouble,
                     .defaults = kOneDefault,
                     .implementation = Implementation::kGoogleSQL,
                 },
                 {
                     .arity = {2, 3},
                     .spelling = "list_transform(bq_bignumeric_generate_array(CAST($1 AS VARCHAR), "
                                 "CAST($2 AS VARCHAR), CAST($3 AS VARCHAR)), lambda _e: CAST(_e AS "
                                 "BIGNUM))",
                     .conditions = kBigNumeric,
                     .defaults = kBigNumericOneDefault,
                     .implementation = Implementation::kGoogleSQL,
                 },
                 {
                     .arity = {2, 3},
                     .spelling = "CASE WHEN $3 = 0 THEN !1 ELSE generate_series($1, $2, $3) END",
                     .conditions = kInt64Sequence,
                     .defaults = kOneDefault,
                     .errors = kSequenceErrors,
                 },
             },
             {
                 .uses = {TypeUse::kBigNumeric},
             }),

      Define("$IS_SOURCE_NODE",
             {
                 {
                     .arity = 2,
                     .spelling = "CASE WHEN $1 IS NULL OR $2 IS NULL THEN !1 ELSE "
                                 "struct_extract($1, '$id') = struct_extract($2, '$source_id') END",
                     .conditions = kEndpoints,
                     .errors = kEndpointErrors,
                 },
             },
             {
                 .uses = {TypeUse::kGraphValues},
             }),

      Define("$IS_DEST_NODE",
             {
                 {
                     .arity = 2,
                     .spelling = "CASE WHEN $1 IS NULL OR $2 IS NULL THEN !1 ELSE "
                                 "struct_extract($1, '$id') = struct_extract($2, "
                                 "'$destination_id') END",
                     .conditions = kEndpoints,
                     .errors = kEndpointErrors,
                 },
             },
             {
                 .uses = {TypeUse::kGraphValues},
             }),

      Define("LABELS",
             {
                 {
                     .arity = 1,
                     .spelling = "struct_extract($1, '$labels')",
                     .conditions = kElement,
                 },
             },
             {
                 .uses = {TypeUse::kGraphValues},
             }),

      Define("NODES",
             {
                 {.arity = 1, .spelling = "struct_extract($1, '$nodes')", .conditions = kPath},
             },
             {
                 .uses = {TypeUse::kGraphValues},
             }),

      Define("EDGES",
             {
                 {.arity = 1, .spelling = "struct_extract($1, '$edges')", .conditions = kPath},
             },
             {
                 .uses = {TypeUse::kGraphValues},
             }),

      Define("PATH_FIRST",
             {
                 {
                     .arity = 1,
                     .spelling = "struct_extract($1, '$nodes')[1]",
                     .conditions = kPath,
                 },
             },
             {
                 .uses = {TypeUse::kGraphValues},
             }),

      Define("PATH_LAST",
             {
                 {
                     .arity = 1,
                     .spelling = "struct_extract($1, '$nodes')[-1]",
                     .conditions = kPath,
                 },
             },
             {
                 .uses = {TypeUse::kGraphValues},
             }),

      Define("PATH_LENGTH",
             {
                 {
                     .arity = 1,
                     .spelling = "len(struct_extract($1, '$edges'))",
                     .conditions = kPath,
                 },
             },
             {
                 .uses = {TypeUse::kGraphValues},
             }),

      Define(
          "IS_ACYCLIC",
          {
              {
                  .arity = 1,
                  .spelling = "list_unique(list_transform(struct_extract($1, '$nodes'), lambda n: "
                              "struct_extract(n, '$id'))) = len(struct_extract($1, '$nodes'))",
                  .conditions = kPath,
              },
          },
          {
              .uses = {TypeUse::kGraphValues},
          }),

      Define(
          "IS_TRAIL",
          {
              {
                  .arity = 1,
                  .spelling = "list_unique(list_transform(struct_extract($1, '$edges'), lambda e: "
                              "struct_extract(e, '$id'))) = len(struct_extract($1, '$edges'))",
                  .conditions = kPath,
              },
          },
          {
              .uses = {TypeUse::kGraphValues},
          }),

      Define(
          "IS_SIMPLE",
          {
              {
                  .arity = 1,
                  .spelling = "list_unique(list_transform(struct_extract($1, '$nodes'), lambda n: "
                              "struct_extract(n, '$id'))) = len(struct_extract($1, '$nodes')) - "
                              "CASE "
                              "WHEN "
                              "len(struct_extract($1, '$nodes')) > 1 AND "
                              "struct_extract(struct_extract($1, '$nodes')[1], '$id') = "
                              "struct_extract(struct_extract($1, '$nodes')[-1], '$id') THEN 1 ELSE "
                              "0 END",
                  .conditions = kPath,
              },
          },
          {
              .uses = {TypeUse::kGraphValues},
          }),

      Define("IF",
             {
                 {.arity = 3, .spelling = "CASE WHEN $1 THEN $2 ELSE $3 END"},
             },
             {
                 .uses = {TypeUse::kCarries, TypeUse::kGraphValues, TypeUse::kBigNumeric},
             }),
  };
  static constexpr auto kRules = DefinitionViews(kRulesBacking);
  return kRules;
}

// Functions implemented by DuckDB SQL templates, including runtime-generated spellings.
std::span<const function_catalog::NamedDefinition<Rule>> TemplateRules() {
  static const function_catalog::NamedDefinition<Rule> kRules[] = {
      // Avoid DuckDB v2's NULLIF macro binder; repeated arguments are bound once by Expand.
      {
          "NULLIF",
          {
              .rules =
                  {
                      {
                          .arity = 2,
                          .spelling = "CASE WHEN " + FloatCompare("$1", "=", "$2") +
                                      " THEN NULL ELSE $1 END",
                          .conditions = {Is(1, {TYPE_DOUBLE})},
                      },
                      {.arity = 2, .spelling = "CASE WHEN $1 = $2 THEN NULL ELSE $1 END"},
                  },
              .uses =
                  {
                      TypeUse::kCompares,
                      TypeUse::kBigNumeric,
                  },
              .struct_handler = StructComparison,
              .type_rejections =
                  {
                      {
                          .types = kIntervalTypes,
                          .message = "Unsupported type in function NULL_IF",
                      },
                  },
          },
      },
      {
          "$EQUAL",
          {
              .rules = Compare("="),
              .uses =
                  {
                      TypeUse::kCompares,
                      TypeUse::kComparesIntervalKeys,
                      TypeUse::kComparesCollationKeys,
                      TypeUse::kComparesStructsWithInterval,
                      TypeUse::kBigNumeric,
                  },
              .struct_handler = StructComparison,
          },
      },
      {
          "$NOT_EQUAL",
          {
              .rules = Compare("<>"),
              .uses =
                  {
                      TypeUse::kCompares,
                      TypeUse::kComparesIntervalKeys,
                      TypeUse::kComparesCollationKeys,
                      TypeUse::kComparesStructsWithInterval,
                      TypeUse::kBigNumeric,
                  },
              .struct_handler = StructComparison,
          },
      },
      {
          "$LESS",
          {
              .rules = Compare("<"),
              .uses =
                  {
                      TypeUse::kCompares,
                      TypeUse::kComparesIntervalKeys,
                      TypeUse::kComparesCollationKeys,
                      TypeUse::kBigNumeric,
                  },
          },
      },
      {
          "$LESS_OR_EQUAL",
          {
              .rules = Compare("<="),
              .uses =
                  {
                      TypeUse::kCompares,
                      TypeUse::kComparesIntervalKeys,
                      TypeUse::kComparesCollationKeys,
                      TypeUse::kBigNumeric,
                  },
          },
      },
      {
          "$GREATER",
          {
              .rules = Compare(">"),
              .uses =
                  {
                      TypeUse::kCompares,
                      TypeUse::kComparesIntervalKeys,
                      TypeUse::kComparesCollationKeys,
                      TypeUse::kBigNumeric,
                  },
          },
      },
      {
          "$GREATER_OR_EQUAL",
          {
              .rules = Compare(">="),
              .uses =
                  {
                      TypeUse::kCompares,
                      TypeUse::kComparesIntervalKeys,
                      TypeUse::kComparesCollationKeys,
                      TypeUse::kBigNumeric,
                  },
          },
      },
      {
          "$BETWEEN",
          {
              .rules =
                  {
                      {
                          .arity = 3,
                          .spelling = "(" + FloatCompare("$1", ">=", "$2") + " AND " +
                                      FloatCompare("$1", "<=", "$3") + ")",
                          .conditions = {Is(1, {TYPE_DOUBLE})},
                      },
                      {.arity = 3, .spelling = "($1 BETWEEN $2 AND $3)"},
                  },
              .uses =
                  {
                      TypeUse::kCompares,
                      TypeUse::kComparesIntervalKeys,
                      TypeUse::kComparesCollationKeys,
                      TypeUse::kBigNumeric,
                  },
          },
      },
      {"$BITWISE_LEFT_SHIFT", {.rules = Shift("<<")}},
      {"$BITWISE_RIGHT_SHIFT", {.rules = Shift(">>")}},
      {
          "$ARRAY_AT_OFFSET",
          {
              .rules = ArrayAt(false, false),
              .uses = {TypeUse::kCarries, TypeUse::kGraphValues, TypeUse::kBigNumeric},
          },
      },
      {
          "$ARRAY_AT_ORDINAL",
          {
              .rules = ArrayAt(true, false),
              .uses = {TypeUse::kCarries, TypeUse::kGraphValues, TypeUse::kBigNumeric},
          },
      },
      {
          "$SAFE_ARRAY_AT_OFFSET",
          {
              .rules = ArrayAt(false, true),
              .uses = {TypeUse::kCarries, TypeUse::kGraphValues, TypeUse::kBigNumeric},
          },
      },
      {
          "$SAFE_ARRAY_AT_ORDINAL",
          {
              .rules = ArrayAt(true, true),
              .uses = {TypeUse::kCarries, TypeUse::kGraphValues, TypeUse::kBigNumeric},
          },
      },

      // DuckDB takes the digits as an INTEGER.
      // DuckDB rounds halfway values away from zero, and its round_even() goes through
      // DOUBLE, so ROUND_HALF_EVEN takes the truncated value instead at a tie whose truncated
      // value is even. Only NUMERIC and BIGNUMERIC take a rounding mode.
      {
          "ROUND",
          {
              .rules = Concat({
                  BigNumericRounding("bq_bignumeric_round", true),
                  {
                      {.arity = 1, .spelling = "round($1)"},
                      {.arity = 2, .spelling = RoundWithDigits("round")},
                      {
                          .arity = 3,
                          .spelling = RoundWithDigits("round"),
                          .conditions = {Mode(3, "ROUND_HALF_AWAY_FROM_ZERO")},
                      },
                      {
                          .arity = 3,
                          .spelling = RoundHalfEven(),
                          .conditions = {Mode(3, "ROUND_HALF_EVEN")},
                      },
                  },
              }),
              .uses = {TypeUse::kBigNumeric},
          },
      },
      {
          "TRUNC",
          {
              .rules = Concat({
                  BigNumericRounding("bq_bignumeric_trunc", false),
                  {
                      {.arity = 1, .spelling = "trunc($1)"},
                      // DuckDB's DOUBLE scaling can round across an integer boundary before
                      // truncation; guards cannot repair that loss of precision. GoogleSQL
                      // computes the decimal scaling in double-double precision instead.
                      {
                          .arity = 2,
                          .spelling = "bq_trunc_digits($1, $2)",
                          .conditions = {Is(1, {TYPE_DOUBLE})},
                          .implementation = Implementation::kGoogleSQL,
                      },
                      {.arity = 2, .spelling = RoundWithDigits("trunc")},
                  },
              }),
              .uses = {TypeUse::kBigNumeric},
          },
      },
      {"CEIL", {.rules = IntegralRound("ceil"), .uses = {TypeUse::kBigNumeric}}},
      {"CEILING", {.rules = IntegralRound("ceil"), .uses = {TypeUse::kBigNumeric}}},
      {"FLOOR", {.rules = IntegralRound("floor"), .uses = {TypeUse::kBigNumeric}}},

      // Date and time arithmetic: DuckDB uses the operators and puts the date part first, as a
      // string rather than as a keyword.
      // DATE_ADD and DATE_SUB also take a DATETIME or a TIMESTAMP, and return its type.
      {
          "DATE_ADD",
          {
              .rules = Concat({
                  WithInterval("CAST($1 + $2 AS DATE)", kDateRange, {Is(1, {TYPE_DATE})}),
                  WithInterval("($1 + $2)", kDatetimeRange, {Is(1, {TYPE_DATETIME})}),
                  WithInterval("($1 + $2)", kTimestampRange, {Is(1, {TYPE_TIMESTAMP})}),
              }),
          },
      },
      {
          "DATE_SUB",
          {
              .rules = Concat({
                  WithInterval("CAST($1 - $2 AS DATE)", kDateRange, {Is(1, {TYPE_DATE})}),
                  WithInterval("($1 - $2)", kDatetimeRange, {Is(1, {TYPE_DATETIME})}),
                  WithInterval("($1 - $2)", kTimestampRange, {Is(1, {TYPE_TIMESTAMP})}),
              }),
          },
      },
      {"DATETIME_ADD", {.rules = WithInterval("($1 + $2)", kDatetimeRange)}},
      {"DATETIME_SUB", {.rules = WithInterval("($1 - $2)", kDatetimeRange)}},
      {"TIMESTAMP_ADD", {.rules = WithInterval("($1 + $2)", kTimestampRange)}},
      {"TIMESTAMP_SUB", {.rules = WithInterval("($1 - $2)", kTimestampRange)}},
      // TIME wraps around midnight.
      {"TIME_ADD", {.rules = TimeArithmetic("+")}},
      {"TIME_SUB", {.rules = TimeArithmetic("-")}},
      {"$EXTRACT", {.rules = Extract(), .uses = {TypeUse::kBuildsOrReadsInterval}}},
      {"GENERATE_TIMESTAMP_ARRAY", {.rules = GenerateTimestampArray()}},
      // DATE and DATETIME differences count the boundaries crossed, which is DuckDB's date_diff,
      // while TIMESTAMP differences below a day count whole units, which is its date_sub. Its
      // date_sub of TIMESTAMPTZ goes through milliseconds, so TIMESTAMPs are compared as UTC
      // civil times.
      {
          "DATE_DIFF",
          {
              .rules = Concat({
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
      },
      {
          "DATETIME_DIFF",
          {.rules = Concat({WeekDiff(), {{.arity = 3, .spelling = "date_diff(#3, $2, $1)"}}})},
      },
      // TIMESTAMP differences count whole units.
      {
          "TIMESTAMP_DIFF",
          {
              .rules = Concat({
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
      },
      {
          "TIME_DIFF",
          {.rules = Concat({WeekDiff(), {{.arity = 3, .spelling = "date_diff(#3, $2, $1)"}}})},
      },
      // date_trunc() widens a DATE to a TIMESTAMP, which DATE_TRUNC does not. DATE_TRUNC also
      // takes a DATETIME or a TIMESTAMP, and returns its type.
      {
          "DATE_TRUNC",
          {
              .rules = Concat({
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
      },
      {
          "DATETIME_TRUNC",
          {
              .rules = Concat(
                  {WeekTrunc(), {{.arity = 2, .spelling = "date_trunc(#2, $1)"}}, ZonedTrunc()}),
          },
      },
      {
          "TIMESTAMP_TRUNC",
          {
              .rules = Concat(
                  {WeekTrunc(), {{.arity = 2, .spelling = "date_trunc(#2, $1)"}}, ZonedTrunc()}),
          },
      },
      {"LAST_DAY", {.rules = LastDay()}},

      // Constructors and conversions between the civil types and TIMESTAMP.
      {"$EXTRACT_DATE", {.rules = Civil("DATE")}},
      {"$EXTRACT_TIME", {.rules = Civil("TIME")}},
      {"$EXTRACT_DATETIME", {.rules = Civil("TIMESTAMP")}},
      {
          "DATE",
          {.rules = Concat({{{.arity = 3, .spelling = "make_date($1, $2, $3)"}}, Civil("DATE")})},
      },
      {
          "TIME",
          {.rules = Concat({{{.arity = 3, .spelling = "make_time($1, $2, $3)"}}, Civil("TIME")})},
      },
      {
          "DATETIME",
          {
              .rules = Concat({
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
      },

      // Epoch conversions. The DuckDB functions return a civil timestamp, which is read as UTC
      // to arrive at the instant BigQuery means.
      {"TIMESTAMP_SECONDS", {.rules = FromUnixTime("to_timestamp($1)", 1000000)}},
      {"TIMESTAMP_MILLIS", {.rules = FromUnixTime("(epoch_ms($1) AT TIME ZONE 'UTC')", 1000)}},
      {"TIMESTAMP_MICROS", {.rules = FromUnixTime("(make_timestamp($1) AT TIME ZONE 'UTC')", 1)}},
      {
          "RANGE_BUCKET",
          {
              .rules = RangeBucket(),
              .uses = {TypeUse::kComparesIntervalKeys, TypeUse::kBigNumeric},
              .argument_modes = {ArgumentMode::kExpression, ArgumentMode::kIntervalArrayKeys},
              .type_rejections =
                  {
                      {
                          .types = kRangeTypes,
                          .message = "Unsupported type RANGE for function RANGE_BUCKET",
                      },
                  },
          },
      },
      {"RANGE", {.rules = RangeConstructor(), .uses = {TypeUse::kBuildsOrReadsRange}}},
      {"RANGE_START", {.rules = BoundOrNull(kRangeStart), .uses = {TypeUse::kBuildsOrReadsRange}}},
      {"RANGE_END", {.rules = BoundOrNull(kRangeEnd), .uses = {TypeUse::kBuildsOrReadsRange}}},
      {"RANGE_CONTAINS", {.rules = RangeContains(), .uses = {TypeUse::kBuildsOrReadsRange}}},
      {
          "RANGE_OVERLAPS",
          {.rules = {{.arity = 2, .spelling = Overlap()}}, .uses = {TypeUse::kBuildsOrReadsRange}},
      },
      {"RANGE_INTERSECT", {.rules = RangeIntersect(), .uses = {TypeUse::kBuildsOrReadsRange}}},
  };
  return kRules;
}

}  // namespace bigquery_emulator_duckdb::translator
