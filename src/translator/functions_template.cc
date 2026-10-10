#include "src/translator/functions_template.h"

#include <cstddef>
#include <initializer_list>
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

// BigQuery weeks start on Sunday. DuckDB's week is the ISO week, which starts on Monday.
std::string WeekStart(const std::string& value, bool iso) {
  return iso ? "date_trunc('week', " + value + ")"
             : "(date_trunc('week', " + value + " + INTERVAL 1 DAY) - INTERVAL 1 DAY)";
}

// *_TRUNC to a week, with the result cast to `target` when date_trunc() would widen it.
std::vector<Rule> WeekTrunc(std::string_view target = "") {
  std::vector<Rule> rules;
  for (const bool iso : {false, true}) {
    const std::string start = WeekStart("$1", iso);
    rules.push_back({
        2,
        target.empty() ? start : "CAST(" + start + " AS " + std::string(target) + ")",
        {Part(2, {iso ? "isoweek" : "week"})},
    });
  }
  return rules;
}

// BigQuery counts the week boundaries crossed, DuckDB whole seven day periods.
std::vector<Rule> WeekDiff() {
  std::vector<Rule> rules;
  for (const bool iso : {false, true}) {
    rules.push_back({
        3,
        "(date_diff('day', " + WeekStart("$2", iso) + ", " + WeekStart("$1", iso) + ") // 7)",
        {Part(3, {iso ? "isoweek" : "week"})},
    });
  }
  return rules;
}

// A cast to a civil type, where a time zone argument moves a TIMESTAMP to the civil time there.
std::vector<Rule> Civil(const std::string& target) {
  // DuckDB cannot cast a TIMESTAMPTZ to TIME, so it goes through the civil time in UTC, the
  // session's time zone.
  return {
      {1, "CAST(CAST($1 AS TIMESTAMP) AS " + target + ")", {Is(1, {TYPE_TIMESTAMP})}},
      {1, "CAST($1 AS " + target + ")"},
      {2, "CAST(timezone($2, $1) AS " + target + ")", {Is(1, {TYPE_TIMESTAMP})}},
  };
}

// CODE_POINTS_TO_STRING and CODE_POINTS_TO_BYTES, by the type `result` they return. They are NULL
// when an element is NULL and raise an error for the first element out of range.
std::vector<Rule> CodePointsTo(googlesql::TypeKind result) {
  const bool bytes = result == TYPE_BYTES;
  const std::string first =
      std::string("list_filter($1, _p -> ") +
      (bytes ? "_p < 0 OR _p > 255" : "_p < 0 OR _p > 1114111 OR _p BETWEEN 55296 AND 57343") +
      ")[1]";
  const std::string spelling =
      bytes ? "unhex(array_to_string(list_transform($1, _p -> lpad(hex(_p), 2, '0')), ''))"
            : "array_to_string(list_transform($1, _p -> chr(CAST(_p AS INTEGER))), '')";
  return {
      {
          1,
          "CASE WHEN list_count($1) <> len($1) THEN NULL WHEN " + first +
              " IS NOT NULL THEN !1 ELSE " + spelling + " END",
          {},
          {},
          {std::string(bytes ? "'Invalid ASCII value '" : "'Invalid codepoint '") + " || " + first},
      },
  };
}

// RANGE_BUCKET counts boundaries <= the point, including duplicates. GoogleSQL implements it
// only in the reference evaluator; DuckDB comparisons suffice for these scalar types once NULL,
// NaN and descending boundaries have been rejected. Templates bind both arguments once.
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
        2,
        std::move(spelling),
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
                          }),
        },
        {},
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
  return {{1, "CASE WHEN isfinite(" + value + ") THEN " + value + " END"}};
}

// A NULL bound is unbounded, and a RANGE whose start does not precede its end an error, with
// BigQuery's message.
std::vector<Rule> RangeConstructor() {
  return {
      {
          2,
          DuckDbRange("$1", "$2", "!1"),
          {},
          {},
          {QuoteLiteral(std::string(kRangeOrderError) + "; error in RANGE expression")},
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
          2,
          "(" + RangeBound("$1", kRangeStart) + " <= " + RangeBound("$2", kRangeStart) + " AND " +
              RangeBound("$2", kRangeEnd) + " <= " + RangeBound("$1", kRangeEnd) + ")",
          {Is(2, {TYPE_RANGE})},
      },
      {
          2,
          "(" + RangeBound("$1", kRangeStart) + " <= $2 AND $2 < " + RangeBound("$1", kRangeEnd) +
              ")",
      },
  };
}

// BigQuery's message, which unlike GoogleSQL's leaves out the ranges.
std::vector<Rule> RangeIntersect() {
  return {
      {
          2,
          "CASE WHEN $1 IS NULL OR $2 IS NULL THEN NULL WHEN NOT " + Overlap() +
              " THEN !1 ELSE struct_pack(" + QuoteIdentifier(kRangeStart) + " := greatest(" +
              RangeBound("$1", kRangeStart) + ", " + RangeBound("$2", kRangeStart) + "), " +
              QuoteIdentifier(kRangeEnd) + " := least(" + RangeBound("$1", kRangeEnd) + ", " +
              RangeBound("$2", kRangeEnd) + ")) END",
          {},
          {},
          {
              "'Provided RANGE inputs do not overlap. Please check RANGE_OVERLAPS before calling "
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
// INTERVAL n PART resolves to two arguments, the count and the date part. The three argument
// form of the date arithmetic functions is the two argument `spelling` of an INTERVAL value with
// $2 multiplied out to that part.
std::vector<Rule> WithInterval(const std::string& spelling) {
  std::vector<Rule> rules = {{2, spelling}};
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
    rules.push_back({3, expanded, {Part(3, {part})}});
  }
  return rules;
}

// EXTRACT, where a time zone argument moves a TIMESTAMP to the civil time there.
std::vector<Rule> Extract() {
  std::vector<Rule> rules = {
      {
          2,
          "date_part(#2, $1)",
          {Is(1, {TYPE_INTERVAL}), Part(2, {"year", "month", "day", "hour", "minute", "second"})},
      },
      {2, "(date_part(#2, $1) % 1000)", {Is(1, {TYPE_INTERVAL}), Part(2, {"millisecond"})}},
      {2, "(date_part(#2, $1) % 1000000)", {Is(1, {TYPE_INTERVAL}), Part(2, {"microsecond"})}},
  };
  for (const auto& [arity, value] :
       {std::pair<std::size_t, std::string>{2, "$1"}, {3, "timezone($3, $1)"}}) {
    rules.push_back({
        arity,
        "(date_part('dayofweek', " + value + ") + 1)",
        {Is(1, {TYPE_DATE, TYPE_TIME, TYPE_DATETIME, TYPE_TIMESTAMP}), Part(2, {"dayofweek"})},
    });
    rules.push_back({
        arity,
        "CAST(strftime(" + value + ", '%U') AS BIGINT)",
        {Is(1, {TYPE_DATE, TYPE_TIME, TYPE_DATETIME, TYPE_TIMESTAMP}), Part(2, {"week"})},
    });
    rules.push_back({
        arity,
        "date_part('week', " + value + ")",
        {Is(1, {TYPE_DATE, TYPE_TIME, TYPE_DATETIME, TYPE_TIMESTAMP}), Part(2, {"isoweek"})},
    });
    // DuckDB counts these from the start of the minute, BigQuery from the start of the second.
    rules.push_back({
        arity,
        "(date_part(#2, " + value + ") % 1000)",
        {Is(1, {TYPE_DATE, TYPE_TIME, TYPE_DATETIME, TYPE_TIMESTAMP}), Part(2, {"millisecond"})},
    });
    rules.push_back({
        arity,
        "(date_part(#2, " + value + ") % 1000000)",
        {Is(1, {TYPE_DATE, TYPE_TIME, TYPE_DATETIME, TYPE_TIMESTAMP}), Part(2, {"microsecond"})},
    });
    rules.push_back({
        arity,
        "date_part(#2, " + value + ")",
        {Is(1, {TYPE_DATE, TYPE_TIME, TYPE_DATETIME, TYPE_TIMESTAMP})},
    });
  }
  return rules;
}

// DuckDB's generate_series steps from the previous element, which only agrees with BigQuery
// stepping from the start for parts of a fixed length.
std::vector<Rule> GenerateDateArray() {
  const auto series = [](const std::string& step) {
    return "list_transform(generate_series(CAST($1 AS TIMESTAMP), CAST($2 AS TIMESTAMP), " + step +
           "), _d -> CAST(_d AS DATE))";
  };
  return {
      {2, series("INTERVAL 1 DAY")},
      {4, series("($3 * INTERVAL '1 day')"), {Part(4, {"day"})}},
      {4, series("($3 * INTERVAL '1 week')"), {Part(4, {"week"})}},
  };
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
        4,
        "list_transform([struct_pack(a := $1, b := $2, s := $3 * " + micros +
            ")], _g -> CASE WHEN _g.s = 0 THEN !1 "
            "ELSE generate_series(_g.a, _g.b, to_microseconds(_g.s)) END)[1]",
        {Part(4, {part})},
        {},
        {"'Sequence step cannot be 0.'"},
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
          2,
          "CASE WHEN $1 IS NULL OR $2 IS NULL THEN NULL WHEN " + out_of_range + " THEN " +
              (safe ? "NULL" : "!1") + " ELSE " + (ordinal ? "$1[$2]" : "$1[$2 + 1]") + " END",
          {},
          {},
          {"'Array index ' || $2 || ' is out of bounds'"},
      },
  };
}

// DuckDB's integer shifts fail on overflow and extend the sign; BigQuery's drop the bits shifted
// out and fill with zeros, which is what shifting a 64-bit BIT string does.
std::vector<Rule> Shift(std::string_view op) {
  return {
      {
          2,
          "CASE WHEN $2 < 0 THEN !1 WHEN $2 >= 64 THEN 0 ELSE CAST(CAST($1 AS BIT) " +
              std::string(op) + " CAST($2 AS INTEGER) AS BIGINT) END",
          {Is(1, {TYPE_INT64})},
          {},
          {"'Bit shift by a negative value'"},
      },
  };
}

// ROUND and TRUNC of a BIGNUMERIC, which take a number of digits, and with `modes` a rounding
// mode.
std::vector<Rule> BigNumericRounding(std::string_view function, bool modes) {
  const std::string name(function);
  std::vector<Rule> rules = {
      BigNumericOperator(name, 1),
      {2, BigNumericCall(name + "_digits", 1, 1), {Is(1, {TYPE_BIGNUMERIC})}},
  };
  if (modes) {
    rules.push_back({
        3,
        BigNumericCall(name + "_digits", 1, 1),
        {Is(1, {TYPE_BIGNUMERIC}), Mode(3, "ROUND_HALF_AWAY_FROM_ZERO")},
    });
    rules.push_back({
        3,
        BigNumericCall(name + "_half_even", 1, 1),
        {Is(1, {TYPE_BIGNUMERIC}), Mode(3, "ROUND_HALF_EVEN")},
    });
  }
  return rules;
}

// `left` `op` `right`, where a NaN FLOAT64 compares unequal to everything, itself included, and
// neither less nor greater. DuckDB takes NaN as equal to itself and greater than every number.
std::string FloatCompare(const std::string& left, std::string_view op, const std::string& right) {
  return "CASE WHEN " + left + " IS NULL OR " + right + " IS NULL THEN NULL WHEN isnan(" + left +
         ") OR isnan(" + right + ") THEN " + (op == "<>" ? "true" : "false") + " ELSE (" + left +
         " " + std::string(op) + " " + right + ") END";
}

// A comparison `op`, with FloatCompare's NaN for FLOAT64.
std::vector<Rule> Compare(std::string_view op) {
  return {
      {2, FloatCompare("$1", op, "$2"), {Is(1, {TYPE_DOUBLE})}},
      {2, "($1 " + std::string(op) + " $2)"},
  };
}

}  // namespace

// Functions implemented by DuckDB SQL templates.
const std::unordered_map<std::string_view, std::vector<Rule>>& TemplateRules() {
  static const auto* const kRules = new std::unordered_map<std::string_view, std::vector<Rule>>{
      // Operators. Division binds its operands once, as every rule does, and stays an
      // expression that CASE and IF can short-circuit; DuckDB would return infinity on zero.
      {"$ADD", {BigNumericOperator("bq_bignumeric_add", 2), {2, "($1 + $2)"}}},
      {"$SUBTRACT", {BigNumericOperator("bq_bignumeric_subtract", 2), {2, "($1 - $2)"}}},
      {"$MULTIPLY", {BigNumericOperator("bq_bignumeric_multiply", 2), {2, "($1 * $2)"}}},
      {
          "$DIVIDE",
          {
              BigNumericOperator("bq_bignumeric_divide", 2),
              {
                  2,
                  "CASE WHEN $1 IS NULL OR $2 IS NULL THEN NULL WHEN $2 = 0 THEN !1 ELSE $1 / $2 "
                  "END",
                  {},
                  {},
                  {"'division by zero'"},
              },
          },
      },
      {"$UNARY_MINUS", {BigNumericOperator("bq_bignumeric_negate", 1), {1, "(-$1)"}}},
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
                  3,
                  "(" + FloatCompare("$1", ">=", "$2") + " AND " + FloatCompare("$1", "<=", "$3") +
                      ")",
                  {Is(1, {TYPE_DOUBLE})},
              },
              {3, "($1 BETWEEN $2 AND $3)"},
          },
      },
      // A backslash escapes the character after it, and must not end the pattern. BYTES are
      // unsupported, since DuckDB's LIKE takes only VARCHAR.
      {
          "$LIKE",
          {
              {
                  2,
                  "CASE WHEN regexp_matches($2, '(^|[^\\\\])(\\\\\\\\)*\\\\$') THEN !1 ELSE "
                  "($1 LIKE $2 ESCAPE '\\') END",
                  {Is(1, {TYPE_STRING})},
                  {},
                  {"'LIKE pattern ends with a backslash'"},
              },
          },
      },
      {"$IS_DISTINCT_FROM", {{2, "($1 IS DISTINCT FROM $2)"}}},
      {"$IS_NOT_DISTINCT_FROM", {{2, "($1 IS NOT DISTINCT FROM $2)"}}},
      {"$IS_NULL", {{1, "($1 IS NULL)"}}},
      {"$IS_TRUE", {{1, "($1 IS TRUE)"}}},
      {"$IS_FALSE", {{1, "($1 IS FALSE)"}}},
      {"$NOT", {{1, "(NOT $1)"}}},
      {"$BITWISE_NOT", {{1, "(~$1)"}}},
      {"$BITWISE_AND", {{2, "($1 & $2)"}}},
      {"$BITWISE_OR", {{2, "($1 | $2)"}}},
      {"$BITWISE_XOR", {{2, "xor($1, $2)"}}},
      {"$BITWISE_LEFT_SHIFT", Shift("<<")},
      {"$BITWISE_RIGHT_SHIFT", Shift(">>")},
      // IN over the unnested elements has BigQuery's NULL handling, and is FALSE for a NULL array.
      {"$IN_ARRAY", {{2, "($1 IN (SELECT unnest($2)))"}}},
      {"$ARRAY_AT_OFFSET", ArrayAt(false, false)},
      {"$ARRAY_AT_ORDINAL", ArrayAt(true, false)},
      {"$SAFE_ARRAY_AT_OFFSET", ArrayAt(false, true)},
      {"$SAFE_ARRAY_AT_ORDINAL", ArrayAt(true, true)},

      // A division by zero is an error in BigQuery and +Inf in DuckDB, so SAFE_DIVIDE has to
      // make the zero itself disappear.
      {
          "SAFE_DIVIDE",
          {BigNumericOperator("bq_bignumeric_safe_divide", 2), {2, "($1 / NULLIF($2, 0))"}},
      },
      {"IEEE_DIVIDE", {{2, "(CAST($1 AS DOUBLE) / CAST($2 AS DOUBLE))"}}},
      // DuckDB returns NULL on a zero divisor, and MOD(x, -1) of the smallest INT64 overflows
      // in DuckDB where it is 0 in BigQuery.
      {
          "MOD",
          {
              BigNumericOperator("bq_bignumeric_mod", 2),
              {
                  2,
                  "CASE WHEN $1 IS NULL OR $2 IS NULL THEN NULL WHEN $2 = 0 THEN !1 WHEN $2 = -1 "
                  "THEN 0 "
                  "ELSE mod($1, $2) END",
                  {Is(1, {TYPE_INT64})},
                  {},
                  {"'division by zero: MOD(' || $1 || ', ' || $2 || ')'"},
              },
              {
                  2,
                  "CASE WHEN $1 IS NULL OR $2 IS NULL THEN NULL WHEN $2 = 0 THEN !1 ELSE mod($1, "
                  "$2) END",
                  {},
                  {},
                  {"'division by zero: MOD(' || $1 || ', ' || $2 || ')'"},
              },
          },
      },
      // DuckDB's divide() of DECIMAL values returns a DOUBLE; GoogleSQL computes the exact
      // NUMERIC quotient and checks overflow.
      {
          "DIV",
          {
              BigNumericOperator("bq_bignumeric_div", 2),
              {2, "bq_div_numeric($1, $2)", {Is(1, {TYPE_NUMERIC})}},
              {
                  2,
                  "CASE WHEN $1 IS NULL OR $2 IS NULL THEN NULL WHEN $2 = 0 THEN !1 ELSE "
                  "divide($1, $2) "
                  "END",
                  {Is(1, {TYPE_INT64})},
                  {},
                  {"'division by zero: ' || $1 || ' / ' || $2"},
              },
          },
      },
      // DuckDB's sign() is 0 for NaN.
      {
          "SIGN",
          {
              BigNumericOperator("bq_bignumeric_sign", 1),
              {1, "CASE WHEN isnan($1) THEN $1 ELSE sign($1) END", {Is(1, {TYPE_DOUBLE})}},
              {1, "sign($1)"},
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
                  {1, "round($1)"},
                  {2, "round($1, CAST($2 AS INTEGER))"},
                  {3, "round($1, CAST($2 AS INTEGER))", {Mode(3, "ROUND_HALF_AWAY_FROM_ZERO")}},
                  {3, RoundHalfEven(), {Mode(3, "ROUND_HALF_EVEN")}},
              },
          }),
      },
      {
          "TRUNC",
          Concat({
              BigNumericRounding("bq_bignumeric_trunc", false),
              {{1, "trunc($1)"}, {2, "trunc($1, CAST($2 AS INTEGER))"}},
          }),
      },
      {"ABS", {BigNumericOperator("bq_bignumeric_abs", 1), {1, "abs($1)"}}},
      {"CEIL", {BigNumericOperator("bq_bignumeric_ceil", 1), {1, "ceil($1)"}}},
      {"CEILING", {BigNumericOperator("bq_bignumeric_ceil", 1), {1, "ceil($1)"}}},
      {"FLOOR", {BigNumericOperator("bq_bignumeric_floor", 1), {1, "floor($1)"}}},
      // DuckDB cannot cast an empty BLOB to BIT.
      {
          "BIT_COUNT",
          {
              {1, "bit_count($1)", {Is(1, {TYPE_INT64})}},
              {
                  1,
                  "CASE WHEN octet_length($1) = 0 THEN 0 ELSE bit_count(CAST($1 AS BIT)) END",
                  {Is(1, {TYPE_BYTES})},
              },
          },
      },

      // Date and time arithmetic: DuckDB uses the operators and puts the date part first, as a
      // string rather than as a keyword.
      {"DATE_ADD", WithInterval("CAST($1 + $2 AS DATE)")},
      {"DATE_SUB", WithInterval("CAST($1 - $2 AS DATE)")},
      {"DATETIME_ADD", WithInterval("($1 + $2)")},
      {"DATETIME_SUB", WithInterval("($1 - $2)")},
      {"TIMESTAMP_ADD", WithInterval("($1 + $2)")},
      {"TIMESTAMP_SUB", WithInterval("($1 - $2)")},
      {"TIME_ADD", WithInterval("($1 + $2)")},
      {"TIME_SUB", WithInterval("($1 - $2)")},
      {"$EXTRACT", Extract()},
      {"GENERATE_DATE_ARRAY", GenerateDateArray()},
      {"GENERATE_TIMESTAMP_ARRAY", GenerateTimestampArray()},
      // Below a day, BigQuery counts whole units rather than the boundaries crossed, which is
      // DuckDB's date_sub rather than date_diff.
      {
          "DATE_DIFF",
          Concat({
              WeekDiff(),
              {{3, "date_sub(#3, $2, $1)", {SubDay(3)}}},
              {{3, "date_diff(#3, $2, $1)"}},
          }),
      },
      {
          "DATETIME_DIFF",
          Concat({
              WeekDiff(),
              {{3, "date_sub(#3, $2, $1)", {SubDay(3)}}},
              {{3, "date_diff(#3, $2, $1)"}},
          }),
      },
      // Every TIMESTAMP and TIME difference counts whole units.
      {"TIMESTAMP_DIFF", Concat({WeekDiff(), {{3, "date_sub(#3, $2, $1)"}}})},
      {"TIME_DIFF", Concat({WeekDiff(), {{3, "date_sub(#3, $2, $1)"}}})},
      // date_trunc() widens a DATE to a TIMESTAMP, which DATE_TRUNC does not.
      {"DATE_TRUNC", Concat({WeekTrunc("DATE"), {{2, "CAST(date_trunc(#2, $1) AS DATE)"}}})},
      {"DATETIME_TRUNC", Concat({WeekTrunc(), {{2, "date_trunc(#2, $1)"}}})},
      {"TIMESTAMP_TRUNC", Concat({WeekTrunc(), {{2, "date_trunc(#2, $1)"}}})},
      {"TIME_TRUNC", {{2, "CAST(date_trunc(#2, DATE '1970-01-01' + $1) AS TIME)"}}},
      {"LAST_DAY", {{{1, 2}, "last_day($1)", {Part(2, {"month"})}}}},

      // Constructors and conversions between the civil types and TIMESTAMP.
      {"CURRENT_DATE", {{0, "CURRENT_DATE"}}},
      {"CURRENT_TIME", {{0, "CURRENT_TIME"}}},
      {"CURRENT_TIMESTAMP", {{0, "CURRENT_TIMESTAMP"}}},
      {"CURRENT_DATETIME", {{0, "CAST(CURRENT_TIMESTAMP AS TIMESTAMP)"}}},
      {"$EXTRACT_DATE", Civil("DATE")},
      {"$EXTRACT_TIME", Civil("TIME")},
      {"$EXTRACT_DATETIME", Civil("TIMESTAMP")},
      {"DATE", Concat({{{3, "make_date($1, $2, $3)"}}, Civil("DATE")})},
      {"TIME", Concat({{{3, "make_time($1, $2, $3)"}}, Civil("TIME")})},
      {
          "DATETIME",
          Concat({
              {
                  {6, "make_timestamp($1, $2, $3, $4, $5, $6)"},
                  {2, "($1 + $2)", {Is(1, {TYPE_DATE}), Is(2, {TYPE_TIME})}},
              },
              Civil("TIMESTAMP"),
          }),
      },
      // GoogleSQL reads a STRING, which has a time zone of its own only without the argument.
      {
          "TIMESTAMP",
          {
              {1, "bq_string_to_timestamp($1, false)", {Is(1, {TYPE_STRING})}},
              {2, "bq_string_to_timestamp_in($1, $2)", {Is(1, {TYPE_STRING})}},
              {1, "CAST($1 AS TIMESTAMPTZ)"},
              // A civil time in the given zone.
              {2, "timezone($2, CAST($1 AS TIMESTAMP))", {Is(1, {TYPE_DATE, TYPE_DATETIME})}},
          },
      },

      // Epoch conversions. The DuckDB functions return a civil timestamp, which is read as UTC
      // to arrive at the instant BigQuery means.
      // Both round down, where casting epoch() rounds to the nearest second and epoch_ms()
      // truncates toward zero.
      {"UNIX_SECONDS", {{1, "CAST(epoch(date_trunc('second', $1)) AS BIGINT)"}}},
      {"UNIX_MILLIS", {{1, "epoch_ms(date_trunc('millisecond', $1))"}}},
      {"UNIX_DATE", {{1, "date_diff('day', DATE '1970-01-01', $1)"}}},
      {"TIMESTAMP_MILLIS", {{1, "(epoch_ms($1) AT TIME ZONE 'UTC')"}}},
      {"TIMESTAMP_MICROS", {{1, "(make_timestamp($1) AT TIME ZONE 'UTC')"}}},
      {
          "DATE_FROM_UNIX_DATE",
          {{1, "CAST(DATE '1970-01-01' + to_days(CAST($1 AS INTEGER)) AS DATE)"}},
      },

      // Strings. The other string functions are GoogleSQL's, at least for BYTES; see
      // BackendRules().
      {"LENGTH", {{1, "octet_length($1)", {Is(1, {TYPE_BYTES})}}, {1, "length($1)"}}},
      {
          "BYTE_LENGTH",
          {
              {1, "strlen($1)", {Is(1, {TYPE_STRING})}},
              {1, "octet_length($1)", {Is(1, {TYPE_BYTES})}},
          },
      },
      {"UNICODE", {{1, "CASE WHEN $1 = '' THEN 0 ELSE unicode($1) END", {Is(1, {TYPE_STRING})}}}},
      {"CHR", {{1, "CASE WHEN $1 = 0 THEN '' ELSE chr(CAST($1 AS INTEGER)) END"}}},
      // '.' would skip line breaks without the s flag.
      {
          "TO_CODE_POINTS",
          {
              {
                  1,
                  "list_transform(regexp_extract_all($1, '(?s).'), _c -> CAST(unicode(_c) AS "
                  "BIGINT))",
                  {Is(1, {TYPE_STRING})},
              },
              {
                  1,
                  "list_transform(regexp_extract_all(hex($1), '..'), _b -> CAST('0x' || _b AS "
                  "BIGINT))",
                  {Is(1, {TYPE_BYTES})},
              },
          },
      },
      {"CODE_POINTS_TO_STRING", CodePointsTo(TYPE_STRING)},
      {"CODE_POINTS_TO_BYTES", CodePointsTo(TYPE_BYTES)},
      // Hashes are BYTES in BigQuery and hexadecimal strings in DuckDB.
      {"MD5", {{1, "unhex(md5($1))"}}},
      {"SHA1", {{1, "unhex(sha1($1))"}}},
      {"SHA256", {{1, "unhex(sha256($1))"}}},
      {"TO_HEX", {{1, "lower(hex($1))"}}},
      {"FROM_HEX", {{1, "unhex($1)", {Is(1, {TYPE_STRING})}}}},
      {"TO_BASE64", {{1, "to_base64($1)"}}},
      {"FROM_BASE64", {{1, "from_base64($1)", {Is(1, {TYPE_STRING})}}}},

      {"ERROR", {{1, "!1", {}, {}, {"$1"}}}},
      {"ARRAY_REVERSE", {{1, "list_reverse($1)"}}},
      {"RANGE_BUCKET", RangeBucket()},
      {"RANGE", RangeConstructor()},
      {"RANGE_START", BoundOrNull(kRangeStart)},
      {"RANGE_END", BoundOrNull(kRangeEnd)},
      {"RANGE_CONTAINS", RangeContains()},
      {"RANGE_OVERLAPS", {{2, Overlap()}}},
      {"RANGE_INTERSECT", RangeIntersect()},
      // DuckDB's array_to_string() skips NULL elements and has no NULL text.
      {
          "ARRAY_TO_STRING",
          {
              {2, "array_to_string($1, $2)", {Is(2, {TYPE_STRING})}},
              {
                  3,
                  "array_to_string(list_transform($1, _e -> coalesce(_e, $3)), $2)",
                  {Is(2, {TYPE_STRING})},
              },
              {
                  2,
                  "unhex(array_to_string(list_transform($1, _e -> hex(_e)), hex($2)))",
                  {Is(2, {TYPE_BYTES})},
              },
              {
                  3,
                  "unhex(array_to_string(list_transform($1, _e -> hex(coalesce(_e, $3))), "
                  "hex($2)))",
                  {Is(2, {TYPE_BYTES})},
              },
          },
      },
      // generate_series() has no floating point overload, and returns an empty list for a zero
      // step, which BigQuery rejects. Other types go to GoogleSQL's implementation.
      {
          "GENERATE_ARRAY",
          {
              {{2, 3}, "bq_generate_array_numeric($1, $2, $3)", {Is(1, {TYPE_NUMERIC})}, {"1"}},
              {{2, 3}, "bq_generate_array($1, $2, $3)", {Is(1, {TYPE_DOUBLE})}, {"1"}},
              {
                  {2, 3},
                  "list_transform(bq_bignumeric_generate_array(CAST($1 AS VARCHAR), CAST($2 AS "
                  "VARCHAR), "
                  "CAST($3 AS VARCHAR)), _e -> CAST(_e AS BIGNUM))",
                  {Is(1, {TYPE_BIGNUMERIC})},
                  {"CAST('100000000000000000000000000000000000000' AS BIGNUM)"},
              },
              {
                  {2, 3},
                  "CASE WHEN $3 = 0 THEN !1 ELSE generate_series($1, $2, $3) END",
                  {Is(1, {TYPE_INT64}), Is(2, {TYPE_INT64}), Is(3, {TYPE_INT64})},
                  {"1"},
                  {"'Sequence step cannot be 0.'"},
              },
          },
      },
  };
  return *kRules;
}

}  // namespace bigquery_emulator_duckdb::translator
