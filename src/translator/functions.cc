#include "src/translator/functions.h"

#include <cstddef>
#include <cstdlib>
#include <initializer_list>
#include <iostream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "src/translator/internal.h"

namespace bigquery_emulator_duckdb::translator {
namespace {

using enum googlesql::TypeKind;

Condition Is(std::size_t argument, std::initializer_list<googlesql::TypeKind> types) {
  return {.argument = argument, .types = types};
}

Condition Part(std::size_t argument, std::initializer_list<std::string_view> date_parts) {
  return {.argument = argument, .date_parts = date_parts};
}

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
    rules.push_back({2,
                     target.empty() ? start : "CAST(" + start + " AS " + std::string(target) + ")",
                     {Part(2, {iso ? "isoweek" : "week"})}});
  }
  return rules;
}

// BigQuery counts the week boundaries crossed, DuckDB whole seven day periods.
std::vector<Rule> WeekDiff() {
  std::vector<Rule> rules;
  for (const bool iso : {false, true}) {
    rules.push_back(
        {3,
         "(date_diff('day', " + WeekStart("$2", iso) + ", " + WeekStart("$1", iso) + ") // 7)",
         {Part(3, {iso ? "isoweek" : "week"})}});
  }
  return rules;
}

// A cast to a civil type, where a time zone argument moves a TIMESTAMP to the civil time there.
std::vector<Rule> Civil(const std::string& target) {
  // DuckDB cannot cast a TIMESTAMPTZ to TIME, so it goes through the civil time in UTC, the
  // session's time zone.
  return {{1, "CAST(CAST($1 AS TIMESTAMP) AS " + target + ")", {Is(1, {TYPE_TIMESTAMP})}},
          {1, "CAST($1 AS " + target + ")"},
          {2, "CAST(timezone($2, $1) AS " + target + ")", {Is(1, {TYPE_TIMESTAMP})}}};
}

// The DuckDB function of the same name, called with each argument count in `arity`, when the
// conditions hold.
std::vector<Rule> Same(std::string_view function, Arity arity,
                       const std::vector<Condition>& conditions) {
  std::vector<Rule> rules;
  for (std::size_t count = arity.min; count <= arity.max; ++count) {
    std::string spelling = std::string(function) + "(";
    for (std::size_t i = 1; i <= count; ++i) {
      spelling += (i == 1 ? "$" : ", $") + std::to_string(i);
    }
    rules.push_back({count, spelling + ")", conditions});
  }
  return rules;
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
      {1,
       "CASE WHEN list_count($1) <> len($1) THEN NULL WHEN " + first +
           " IS NOT NULL THEN !1 ELSE " + spelling + " END",
       {},
       {},
       {std::string(bytes ? "'Invalid ASCII value '" : "'Invalid codepoint '") + " || " + first}}};
}

// SUBSTR and SUBSTRING. BigQuery starts at the first character for a position of 0 or one
// before the start, where DuckDB takes as many characters fewer. BYTES go to GoogleSQL, without
// a length taking the rest.
std::vector<Rule> Substr() {
  const std::string start =
      "CASE WHEN $2 > 0 THEN $2 WHEN $2 = 0 OR $2 < -length($1) THEN 1 ELSE length($1) + $2 + 1 "
      "END";
  return {{2, "substr($1, " + start + ")", {Is(1, {TYPE_STRING})}},
          {3,
           "CASE WHEN $3 < 0 THEN !1 ELSE substr($1, " + start + ", $3) END",
           {Is(1, {TYPE_STRING})},
           {},
           {"'Third argument in SUBSTR() cannot be negative'"}},
          {{2, 3}, "bq_substr_bytes($1, $2, $3)", {Is(1, {TYPE_BYTES})}, {"9223372036854775807"}}};
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
    rules.push_back(
        {2,
         std::move(spelling),
         {floating ? Is(1, {TYPE_DOUBLE})
                   : Is(1, {TYPE_INT64, TYPE_NUMERIC, TYPE_BIGNUMERIC, TYPE_BOOL, TYPE_STRING,
                            TYPE_BYTES, TYPE_DATE, TYPE_TIME, TYPE_DATETIME, TYPE_TIMESTAMP})},
         {},
         {"'Elements in input array to RANGE_BUCKET cannot be null.'",
          "'Elements in input array to RANGE_BUCKET cannot be NaN.'",
          "'Elements in input array to RANGE_BUCKET must be in ascending order.'"}});
  }
  return rules;
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

// FORMAT_DATE, FORMAT_DATETIME and FORMAT_TIMESTAMP, by the type of the value. A TIMESTAMP is
// formatted in the given time zone, by default UTC.
std::vector<Rule> FormatDateTime() {
  return {{2, "bq_format_date($1, $2)", {Is(2, {TYPE_DATE})}},
          {2, "bq_format_datetime($1, $2)", {Is(2, {TYPE_DATETIME})}},
          {{2, 3}, "bq_format_timestamp($1, $2, $3)", {Is(2, {TYPE_TIMESTAMP})}, {"'UTC'"}}};
}

// A FLOAT64 function of `arity` arguments that src/backend_functions.cc registers.
std::vector<Rule> Float64(std::string_view function, std::size_t arity = 1) {
  return Same(function, arity, {Is(1, {TYPE_DOUBLE})});
}

// INTERVAL n PART resolves to two arguments, the count and the date part. The three argument
// form of the date arithmetic functions is the two argument `spelling` of an INTERVAL value with
// $2 multiplied out to that part.
std::vector<Rule> WithInterval(const std::string& spelling) {
  std::vector<Rule> rules = {{2, spelling}};
  for (const std::string_view part : {"year", "quarter", "month", "week", "day", "hour", "minute",
                                      "second", "millisecond", "microsecond"}) {
    std::string expanded;
    for (std::size_t i = 0; i < spelling.size(); ++i) {
      if (spelling.compare(i, 2, "$2") == 0) {
        expanded += "($2 * INTERVAL '1 " + std::string(part) + "')";
        ++i;
      } else {
        expanded += spelling[i];
      }
    }
    rules.push_back({3, expanded, {Part(3, {part})}});
  }
  return rules;
}

// EXTRACT, where a time zone argument moves a TIMESTAMP to the civil time there.
std::vector<Rule> Extract() {
  std::vector<Rule> rules;
  for (const auto& [arity, value] :
       {std::pair<std::size_t, std::string>{2, "$1"}, {3, "timezone($3, $1)"}}) {
    rules.push_back(
        {arity, "(date_part('dayofweek', " + value + ") + 1)", {Part(2, {"dayofweek"})}});
    rules.push_back({arity, "CAST(strftime(" + value + ", '%U') AS BIGINT)", {Part(2, {"week"})}});
    rules.push_back({arity, "date_part('week', " + value + ")", {Part(2, {"isoweek"})}});
    // DuckDB counts these from the start of the minute, BigQuery from the start of the second.
    rules.push_back({arity, "(date_part(#2, " + value + ") % 1000)", {Part(2, {"millisecond"})}});
    rules.push_back(
        {arity, "(date_part(#2, " + value + ") % 1000000)", {Part(2, {"microsecond"})}});
    rules.push_back({arity, "date_part(#2, " + value + ")"});
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
  return {{2, series("INTERVAL 1 DAY")},
          {4, series("($3 * INTERVAL '1 day')"), {Part(4, {"day"})}},
          {4, series("($3 * INTERVAL '1 week')"), {Part(4, {"week"})}}};
}

// Steps of fixed microseconds, so the session time zone plays no part. DuckDB would return an
// empty array for a zero step.
std::vector<Rule> GenerateTimestampArray() {
  std::vector<Rule> rules;
  for (const auto& [part, micros] :
       std::initializer_list<std::pair<std::string_view, std::string>>{{"day", "86400000000"},
                                                                       {"hour", "3600000000"},
                                                                       {"minute", "60000000"},
                                                                       {"second", "1000000"},
                                                                       {"millisecond", "1000"},
                                                                       {"microsecond", "1"}}) {
    rules.push_back({4,
                     "list_transform([struct_pack(a := $1, b := $2, s := $3 * " + micros +
                         ")], _g -> CASE WHEN _g.s = 0 THEN !1 "
                         "ELSE generate_series(_g.a, _g.b, to_microseconds(_g.s)) END)[1]",
                     {Part(4, {part})},
                     {},
                     {"'Sequence step cannot be 0.'"}});
  }
  return rules;
}

// $ARRAY_AT_OFFSET and its siblings. DuckDB would return NULL for an index out of range, and
// count negative indexes from the end.
std::vector<Rule> ArrayAt(bool ordinal, bool safe) {
  const std::string out_of_range = ordinal ? "$2 < 1 OR $2 > len($1)" : "$2 < 0 OR $2 >= len($1)";
  return {{2,
           "CASE WHEN $1 IS NULL OR $2 IS NULL THEN NULL WHEN " + out_of_range + " THEN " +
               (safe ? "NULL" : "!1") + " ELSE " + (ordinal ? "$1[$2]" : "$1[$2 + 1]") + " END",
           {},
           {},
           {"'Array index ' || $2 || ' is out of bounds'"}}};
}

// DuckDB's integer shifts fail on overflow and extend the sign; BigQuery's drop the bits shifted
// out and fill with zeros, which is what shifting a 64-bit BIT string does.
std::vector<Rule> Shift(std::string_view op) {
  return {{2,
           "CASE WHEN $2 < 0 THEN !1 WHEN $2 >= 64 THEN 0 ELSE CAST(CAST($1 AS BIT) " +
               std::string(op) + " CAST($2 AS INTEGER) AS BIGINT) END",
           {Is(1, {TYPE_INT64})},
           {},
           {"'Bit shift by a negative value'"}}};
}

// JSON_QUERY and the other extractions of `function` in src/backend_functions.cc, of a STRING
// or of JSON. `standard` is false for the legacy JSON_EXTRACT functions' JSONPath.
std::vector<Rule> JsonExtract(std::string_view function, bool standard, bool json_result) {
  const std::string flag = standard ? "true" : "false";
  const std::string of_json =
      std::string(function) + "_json(CAST($1 AS VARCHAR), $2, " + flag + ")";
  return {
      {{1, 2}, json_result ? "json(" + of_json + ")" : of_json, {Is(1, {TYPE_JSON})}, {"'$'"}},
      {{1, 2}, std::string(function) + "($1, $2, " + flag + ")", {Is(1, {TYPE_STRING})}, {"'$'"}}};
}

std::vector<Rule> Concat(std::initializer_list<std::vector<Rule>> groups) {
  std::vector<Rule> rules;
  for (const auto& group : groups) {
    rules.insert(rules.end(), group.begin(), group.end());
  }
  return rules;
}

// A function that src/backend_functions.cc registers as `function` for STRING and as
// `function`_bytes for BYTES.
std::vector<Rule> Strings(std::string_view function, Arity arity) {
  return Concat({Same(function, arity, {Is(1, {TYPE_STRING})}),
                 Same(std::string(function) + "_bytes", arity, {Is(1, {TYPE_BYTES})})});
}

// A call to `function`, a BIGNUMERIC function that src/backend_functions/bignumeric.cc
// implements, which takes and returns the units of a BIGNUM as text. `bignumerics` arguments are
// BIGNUMERIC, and the `others` after them, such as ROUND's digits, go as they are.
std::string BigNumericCall(std::string_view function, std::size_t bignumerics,
                           std::size_t others = 0) {
  std::string arguments;
  for (std::size_t i = 1; i <= bignumerics + others; ++i) {
    const std::string argument = "$" + std::to_string(i);
    arguments +=
        (i == 1 ? "" : ", ") + (i <= bignumerics ? "CAST(" + argument + " AS VARCHAR)" : argument);
  }
  return "CAST(" + std::string(function) + "(" + arguments + ") AS BIGNUM)";
}

// An operator on BIGNUMERIC, all of whose `arity` arguments are BIGNUMERIC.
Rule BigNumericOperator(std::string_view function, std::size_t arity) {
  return {arity, BigNumericCall(function, arity), {Is(1, {TYPE_BIGNUMERIC})}};
}

// A function that src/backend_functions.cc registers as bq_`name` for FLOAT64, as
// bq_`name`_numeric for NUMERIC and as bq_bignumeric_`name` for BIGNUMERIC, the last two of which
// keep their precision rather than going through FLOAT64.
std::vector<Rule> Numbers(std::string_view name, std::size_t arity = 1) {
  const std::string function = "bq_" + std::string(name);
  return Concat({{BigNumericOperator("bq_bignumeric_" + std::string(name), arity)},
                 Float64(function, arity),
                 Same(function + "_numeric", arity, {Is(1, {TYPE_NUMERIC})})});
}

// ROUND and TRUNC of a BIGNUMERIC, which take a number of digits, and with `modes` a rounding
// mode.
std::vector<Rule> BigNumericRounding(std::string_view function, bool modes) {
  const std::string name(function);
  std::vector<Rule> rules = {
      BigNumericOperator(name, 1),
      {2, BigNumericCall(name + "_digits", 1, 1), {Is(1, {TYPE_BIGNUMERIC})}}};
  if (modes) {
    rules.push_back({3,
                     BigNumericCall(name + "_digits", 1, 1),
                     {Is(1, {TYPE_BIGNUMERIC}), Mode(3, "ROUND_HALF_AWAY_FROM_ZERO")}});
    rules.push_back({3,
                     BigNumericCall(name + "_half_even", 1, 1),
                     {Is(1, {TYPE_BIGNUMERIC}), Mode(3, "ROUND_HALF_EVEN")}});
  }
  return rules;
}

// TRIM, LTRIM and RTRIM. Without the characters to trim, they trim Unicode whitespace, where
// DuckDB's trim() only trims spaces.
std::vector<Rule> Trim(const std::string& function) {
  return {{1, function + "($1)", {Is(1, {TYPE_STRING})}},
          {2, function + "_chars($1, $2)", {Is(1, {TYPE_STRING})}},
          {2, function + "_bytes($1, $2)", {Is(1, {TYPE_BYTES})}}};
}

// LPAD and RPAD, which pad with spaces by default.
std::vector<Rule> Pad(const std::string& function) {
  return {{{2, 3}, function + "($1, $2, $3)", {Is(1, {TYPE_STRING})}, {"' '"}},
          {{2, 3}, function + "_bytes($1, $2, $3)", {Is(1, {TYPE_BYTES})}, {"encode(' ')"}}};
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
  return {{2, FloatCompare("$1", op, "$2"), {Is(1, {TYPE_DOUBLE})}},
          {2, "($1 " + std::string(op) + " $2)"}};
}

// Functions implemented by DuckDB SQL templates.
const std::unordered_map<std::string_view, std::vector<Rule>>& TemplateRules() {
  static const auto* const kRules = new std::unordered_map<std::string_view, std::vector<Rule>>{
      // Operators. Division binds its operands once, as every rule does, and stays an
      // expression that CASE and IF can short-circuit; DuckDB would return infinity on zero.
      {"$ADD", {BigNumericOperator("bq_bignumeric_add", 2), {2, "($1 + $2)"}}},
      {"$SUBTRACT", {BigNumericOperator("bq_bignumeric_subtract", 2), {2, "($1 - $2)"}}},
      {"$MULTIPLY", {BigNumericOperator("bq_bignumeric_multiply", 2), {2, "($1 * $2)"}}},
      {"$DIVIDE",
       {BigNumericOperator("bq_bignumeric_divide", 2),
        {2,
         "CASE WHEN $1 IS NULL OR $2 IS NULL THEN NULL WHEN $2 = 0 THEN !1 ELSE $1 / $2 END",
         {},
         {},
         {"'division by zero'"}}}},
      {"$UNARY_MINUS", {BigNumericOperator("bq_bignumeric_negate", 1), {1, "(-$1)"}}},
      {"$EQUAL", Compare("=")},
      {"$NOT_EQUAL", Compare("<>")},
      {"$LESS", Compare("<")},
      {"$LESS_OR_EQUAL", Compare("<=")},
      {"$GREATER", Compare(">")},
      {"$GREATER_OR_EQUAL", Compare(">=")},
      {"$BETWEEN",
       {{3,
         "(" + FloatCompare("$1", ">=", "$2") + " AND " + FloatCompare("$1", "<=", "$3") + ")",
         {Is(1, {TYPE_DOUBLE})}},
        {3, "($1 BETWEEN $2 AND $3)"}}},
      // A backslash escapes the character after it, and must not end the pattern. BYTES are
      // unsupported, since DuckDB's LIKE takes only VARCHAR.
      {"$LIKE",
       {{2,
         "CASE WHEN regexp_matches($2, '(^|[^\\\\])(\\\\\\\\)*\\\\$') THEN !1 ELSE "
         "($1 LIKE $2 ESCAPE '\\') END",
         {Is(1, {TYPE_STRING})},
         {},
         {"'LIKE pattern ends with a backslash'"}}}},
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
      {"SAFE_DIVIDE",
       {BigNumericOperator("bq_bignumeric_safe_divide", 2), {2, "($1 / NULLIF($2, 0))"}}},
      {"IEEE_DIVIDE", {{2, "(CAST($1 AS DOUBLE) / CAST($2 AS DOUBLE))"}}},
      // DuckDB returns NULL on a zero divisor, and MOD(x, -1) of the smallest INT64 overflows
      // in DuckDB where it is 0 in BigQuery.
      {"MOD",
       {BigNumericOperator("bq_bignumeric_mod", 2),
        {2,
         "CASE WHEN $1 IS NULL OR $2 IS NULL THEN NULL WHEN $2 = 0 THEN !1 WHEN $2 = -1 THEN 0 "
         "ELSE mod($1, $2) END",
         {Is(1, {TYPE_INT64})},
         {},
         {"'division by zero: MOD(' || $1 || ', ' || $2 || ')'"}},
        {2,
         "CASE WHEN $1 IS NULL OR $2 IS NULL THEN NULL WHEN $2 = 0 THEN !1 ELSE mod($1, $2) END",
         {},
         {},
         {"'division by zero: MOD(' || $1 || ', ' || $2 || ')'"}}}},
      // DuckDB's divide() of DECIMAL values returns a DOUBLE; GoogleSQL computes the exact
      // NUMERIC quotient and checks overflow.
      {"DIV",
       {BigNumericOperator("bq_bignumeric_div", 2),
        {2, "bq_div_numeric($1, $2)", {Is(1, {TYPE_NUMERIC})}},
        {2,
         "CASE WHEN $1 IS NULL OR $2 IS NULL THEN NULL WHEN $2 = 0 THEN !1 ELSE divide($1, $2) "
         "END",
         {Is(1, {TYPE_INT64})},
         {},
         {"'division by zero: ' || $1 || ' / ' || $2"}}}},
      // DuckDB's sign() is 0 for NaN.
      {"SIGN",
       {BigNumericOperator("bq_bignumeric_sign", 1),
        {1, "CASE WHEN isnan($1) THEN $1 ELSE sign($1) END", {Is(1, {TYPE_DOUBLE})}},
        {1, "sign($1)"}}},
      // DuckDB takes the digits as an INTEGER.
      // DuckDB rounds halfway values away from zero, and its round_even() goes through
      // DOUBLE, so ROUND_HALF_EVEN takes the truncated value instead at a tie whose truncated
      // value is even. Only NUMERIC and BIGNUMERIC take a rounding mode.
      {"ROUND",
       Concat({BigNumericRounding("bq_bignumeric_round", true),
               {{1, "round($1)"},
                {2, "round($1, CAST($2 AS INTEGER))"},
                {3, "round($1, CAST($2 AS INTEGER))", {Mode(3, "ROUND_HALF_AWAY_FROM_ZERO")}},
                {3, RoundHalfEven(), {Mode(3, "ROUND_HALF_EVEN")}}}})},
      {"TRUNC", Concat({BigNumericRounding("bq_bignumeric_trunc", false),
                        {{1, "trunc($1)"}, {2, "trunc($1, CAST($2 AS INTEGER))"}}})},
      {"ABS", {BigNumericOperator("bq_bignumeric_abs", 1), {1, "abs($1)"}}},
      {"CEIL", {BigNumericOperator("bq_bignumeric_ceil", 1), {1, "ceil($1)"}}},
      {"CEILING", {BigNumericOperator("bq_bignumeric_ceil", 1), {1, "ceil($1)"}}},
      {"FLOOR", {BigNumericOperator("bq_bignumeric_floor", 1), {1, "floor($1)"}}},
      // DuckDB cannot cast an empty BLOB to BIT.
      {"BIT_COUNT",
       {{1, "bit_count($1)", {Is(1, {TYPE_INT64})}},
        {1,
         "CASE WHEN octet_length($1) = 0 THEN 0 ELSE bit_count(CAST($1 AS BIT)) END",
         {Is(1, {TYPE_BYTES})}}}},

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
      {"DATE_DIFF", Concat({WeekDiff(),
                            {{3, "date_sub(#3, $2, $1)", {SubDay(3)}}},
                            {{3, "date_diff(#3, $2, $1)"}}})},
      {"DATETIME_DIFF", Concat({WeekDiff(),
                                {{3, "date_sub(#3, $2, $1)", {SubDay(3)}}},
                                {{3, "date_diff(#3, $2, $1)"}}})},
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
      {"DATETIME", Concat({{{6, "make_timestamp($1, $2, $3, $4, $5, $6)"},
                            {2, "($1 + $2)", {Is(1, {TYPE_DATE}), Is(2, {TYPE_TIME})}}},
                           Civil("TIMESTAMP")})},
      // GoogleSQL reads a STRING, which has a time zone of its own only without the argument.
      {"TIMESTAMP",
       {{1, "bq_string_to_timestamp($1, false)", {Is(1, {TYPE_STRING})}},
        {2, "bq_string_to_timestamp_in($1, $2)", {Is(1, {TYPE_STRING})}},
        {1, "CAST($1 AS TIMESTAMPTZ)"},
        // A civil time in the given zone.
        {2, "timezone($2, CAST($1 AS TIMESTAMP))", {Is(1, {TYPE_DATE, TYPE_DATETIME})}}}},

      // Epoch conversions. The DuckDB functions return a civil timestamp, which is read as UTC
      // to arrive at the instant BigQuery means.
      // Both round down, where casting epoch() rounds to the nearest second and epoch_ms()
      // truncates toward zero.
      {"UNIX_SECONDS", {{1, "CAST(epoch(date_trunc('second', $1)) AS BIGINT)"}}},
      {"UNIX_MILLIS", {{1, "epoch_ms(date_trunc('millisecond', $1))"}}},
      {"UNIX_DATE", {{1, "date_diff('day', DATE '1970-01-01', $1)"}}},
      {"TIMESTAMP_MILLIS", {{1, "(epoch_ms($1) AT TIME ZONE 'UTC')"}}},
      {"TIMESTAMP_MICROS", {{1, "(make_timestamp($1) AT TIME ZONE 'UTC')"}}},
      {"DATE_FROM_UNIX_DATE",
       {{1, "CAST(DATE '1970-01-01' + to_days(CAST($1 AS INTEGER)) AS DATE)"}}},

      // Strings. The other string functions are GoogleSQL's, at least for BYTES; see
      // BackendRules().
      {"LENGTH", {{1, "octet_length($1)", {Is(1, {TYPE_BYTES})}}, {1, "length($1)"}}},
      {"BYTE_LENGTH",
       {{1, "strlen($1)", {Is(1, {TYPE_STRING})}}, {1, "octet_length($1)", {Is(1, {TYPE_BYTES})}}}},
      {"UNICODE", {{1, "CASE WHEN $1 = '' THEN 0 ELSE unicode($1) END", {Is(1, {TYPE_STRING})}}}},
      {"CHR", {{1, "CASE WHEN $1 = 0 THEN '' ELSE chr(CAST($1 AS INTEGER)) END"}}},
      // '.' would skip line breaks without the s flag.
      {"TO_CODE_POINTS",
       {{1,
         "list_transform(regexp_extract_all($1, '(?s).'), _c -> CAST(unicode(_c) AS BIGINT))",
         {Is(1, {TYPE_STRING})}},
        {1,
         "list_transform(regexp_extract_all(hex($1), '..'), _b -> CAST('0x' || _b AS BIGINT))",
         {Is(1, {TYPE_BYTES})}}}},
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
      // DuckDB's array_to_string() skips NULL elements and has no NULL text.
      {"ARRAY_TO_STRING",
       {{2, "array_to_string($1, $2)", {Is(2, {TYPE_STRING})}},
        {3,
         "array_to_string(list_transform($1, _e -> coalesce(_e, $3)), $2)",
         {Is(2, {TYPE_STRING})}},
        {2,
         "unhex(array_to_string(list_transform($1, _e -> hex(_e)), hex($2)))",
         {Is(2, {TYPE_BYTES})}},
        {3,
         "unhex(array_to_string(list_transform($1, _e -> hex(coalesce(_e, $3))), hex($2)))",
         {Is(2, {TYPE_BYTES})}}}},
      // generate_series() has no floating point overload, and returns an empty list for a zero
      // step, which BigQuery rejects. Other types go to GoogleSQL's implementation.
      {"GENERATE_ARRAY",
       {{{2, 3}, "bq_generate_array_numeric($1, $2, $3)", {Is(1, {TYPE_NUMERIC})}, {"1"}},
        {{2, 3}, "bq_generate_array($1, $2, $3)", {Is(1, {TYPE_DOUBLE})}, {"1"}},
        {{2, 3},
         "list_transform(bq_bignumeric_generate_array(CAST($1 AS VARCHAR), CAST($2 AS VARCHAR), "
         "CAST($3 AS VARCHAR)), _e -> CAST(_e AS BIGNUM))",
         {Is(1, {TYPE_BIGNUMERIC})},
         {"CAST('100000000000000000000000000000000000000' AS BIGNUM)"}},
        {{2, 3},
         "CASE WHEN $3 = 0 THEN !1 ELSE generate_series($1, $2, $3) END",
         {Is(1, {TYPE_INT64}), Is(2, {TYPE_INT64}), Is(3, {TYPE_INT64})},
         {"1"},
         {"'Sequence step cannot be 0.'"}}}},
  };
  return *kRules;
}

// Functions implemented by templates that call GoogleSQL's own implementations, which
// src/backend_functions.cc registers as bq_* DuckDB functions.
const std::unordered_map<std::string_view, std::vector<Rule>>& BackendRules() {
  static const auto* const kRules = new std::unordered_map<std::string_view, std::vector<Rule>>{
      // DuckDB raises errors where BigQuery returns NaN, such as SIN(+inf), and returns
      // infinities where BigQuery raises errors, such as EXP(1000).
      {"SQRT", Numbers("sqrt")},
      {"CBRT", Numbers("cbrt")},
      {"POW", Numbers("pow", 2)},
      {"POWER", Numbers("pow", 2)},
      {"EXP", Numbers("exp")},
      {"LN", Numbers("ln")},
      {"LOG10", Numbers("log10")},
      {"LOG", Concat({Numbers("ln"), Numbers("log", 2)})},
      {"PARSE_BIGNUMERIC", {{1, "CAST(bq_parse_bignumeric($1) AS BIGNUM)"}}},
      {"SIN", Float64("bq_sin")},
      {"COS", Float64("bq_cos")},
      {"TAN", Float64("bq_tan")},
      {"ASIN", Float64("bq_asin")},
      {"ACOS", Float64("bq_acos")},
      {"SINH", Float64("bq_sinh")},
      {"COSH", Float64("bq_cosh")},
      {"ACOSH", Float64("bq_acosh")},
      {"ATANH", Float64("bq_atanh")},
      {"CSC", Float64("bq_csc")},
      {"SEC", Float64("bq_sec")},
      {"COT", Float64("bq_cot")},
      {"CSCH", Float64("bq_csch")},
      {"SECH", Float64("bq_sech")},
      {"COTH", Float64("bq_coth")},
      // Strings. DuckDB's BLOB has almost no functions, its case mapping is simple rather than
      // full, it reverses grapheme clusters rather than characters, it trims spaces rather than
      // whitespace, and it neither raises BigQuery's errors nor has its output limit. Where it
      // agrees, the STRING overload stays DuckDB's.
      {"REPEAT", Strings("bq_repeat", 2)},
      {"LOWER", Strings("bq_lower", 1)},
      {"UPPER", Strings("bq_upper", 1)},
      {"REVERSE", Strings("bq_reverse", 1)},
      {"REPLACE", Strings("bq_replace", 3)},
      {"TRANSLATE", Strings("bq_translate", 3)},
      {"ASCII", Strings("bq_ascii", 1)},
      {"LEFT", Strings("bq_left", 2)},
      {"RIGHT", Strings("bq_right", 2)},
      {"LPAD", Pad("bq_lpad")},
      {"RPAD", Pad("bq_rpad")},
      {"TRIM", Trim("bq_trim")},
      {"LTRIM", Trim("bq_ltrim")},
      {"RTRIM", Trim("bq_rtrim")},
      {"INSTR",
       {{{2, 4}, "bq_instr($1, $2, $3, $4)", {Is(1, {TYPE_STRING})}, {"1", "1"}},
        {{2, 4}, "bq_instr_bytes($1, $2, $3, $4)", {Is(1, {TYPE_BYTES})}, {"1", "1"}}}},
      {"STRPOS",
       {{2, "strpos($1, $2)", {Is(1, {TYPE_STRING})}},
        {2, "bq_instr_bytes($1, $2, 1, 1)", {Is(1, {TYPE_BYTES})}}}},
      {"STARTS_WITH",
       {{2, "starts_with($1, $2)", {Is(1, {TYPE_STRING})}},
        {2, "bq_starts_with_bytes($1, $2)", {Is(1, {TYPE_BYTES})}}}},
      {"ENDS_WITH",
       {{2, "ends_with($1, $2)", {Is(1, {TYPE_STRING})}},
        {2, "bq_ends_with_bytes($1, $2)", {Is(1, {TYPE_BYTES})}}}},
      {"SUBSTR", Substr()},
      {"SUBSTRING", Substr()},
      {"SPLIT",
       {{{1, 2}, "split($1, $2)", {Is(1, {TYPE_STRING})}, {"','"}},
        {2, "bq_split_bytes($1, $2)", {Is(1, {TYPE_BYTES})}}}},
      // The mode is passed by its name.
      // Formatting and parsing dates and times. DuckDB's strftime() and strptime() differ in
      // the format elements, in how leniently they parse and in their errors. FORMAT_DATE,
      // FORMAT_DATETIME and FORMAT_TIMESTAMP each also take the other two types.
      {"FORMAT_DATE", FormatDateTime()},
      {"FORMAT_DATETIME", FormatDateTime()},
      {"FORMAT_TIMESTAMP", FormatDateTime()},
      {"FORMAT_TIME", {{2, "bq_format_time($1, $2)"}}},
      {"PARSE_DATE", {{2, "bq_parse_date($1, $2)"}}},
      {"PARSE_DATETIME", {{2, "bq_parse_datetime($1, $2)"}}},
      {"PARSE_TIME", {{2, "bq_parse_time($1, $2)"}}},
      // A string without a time zone is in the given one, by default UTC.
      {"PARSE_TIMESTAMP", {{{2, 3}, "bq_parse_timestamp($1, $2, $3)", {}, {"'UTC'"}}}},
      {"NORMALIZE", {{{1, 2}, "bq_normalize($1, $2)", {}, {"'NFC'"}}}},
      {"NORMALIZE_AND_CASEFOLD", {{{1, 2}, "bq_normalize_and_casefold($1, $2)", {}, {"'NFC'"}}}},
      // BigQuery compares the NFKC normal forms, case folded. Only the STRING overload is
      // declared.
      {"CONTAINS_SUBSTR",
       {{2,
         "contains(bq_normalize_and_casefold($1, 'NFKC'), bq_normalize_and_casefold($2, 'NFKC'))",
         {Is(1, {TYPE_STRING}), Is(2, {TYPE_STRING})}}}},
      // encode() takes a STRING's UTF-8 bytes.
      {"SHA512",
       {{1, "bq_sha512(encode($1))", {Is(1, {TYPE_STRING})}},
        {1, "bq_sha512($1)", {Is(1, {TYPE_BYTES})}}}},
      {"FARM_FINGERPRINT",
       {{1, "bq_farm_fingerprint(encode($1))", {Is(1, {TYPE_STRING})}},
        {1, "bq_farm_fingerprint($1)", {Is(1, {TYPE_BYTES})}}}},
      {"INITCAP", {{1, "bq_initcap($1)"}, {2, "bq_initcap_delimiters($1, $2)"}}},
      {"SOUNDEX", {{1, "bq_soundex($1)"}}},
      {"SAFE_CONVERT_BYTES_TO_STRING", {{1, "bq_safe_convert_bytes_to_string($1)"}}},
      // NET functions. HOST, REG_DOMAIN and PUBLIC_SUFFIX follow GoogleSQL's copy of the public
      // suffix list.
      {"IPV4_FROM_INT64", {{1, "bq_net_ipv4_from_int64($1)"}}},
      {"IPV4_TO_INT64", {{1, "bq_net_ipv4_to_int64($1)"}}},
      {"IP_FROM_STRING", {{1, "bq_net_ip_from_string($1)"}}},
      {"SAFE_IP_FROM_STRING", {{1, "bq_net_safe_ip_from_string($1)"}}},
      {"IP_TO_STRING", {{1, "bq_net_ip_to_string($1)"}}},
      {"IP_NET_MASK", {{2, "bq_net_ip_net_mask($1, $2)"}}},
      {"IP_TRUNC", {{2, "bq_net_ip_trunc($1, $2)"}}},
      {"HOST", {{1, "bq_net_host($1)"}}},
      {"REG_DOMAIN", {{1, "bq_net_reg_domain($1)"}}},
      {"PUBLIC_SUFFIX", {{1, "bq_net_public_suffix($1)"}}},
      // AEAD with Tink keysets of AES-GCM keys, where a STRING is its UTF-8 bytes. The keyset
      // chain of KEYS.KEYSET_CHAIN needs Cloud KMS.
      {"ENCRYPT",
       {{3,
         "bq_aead_encrypt($1, encode($2), encode($3))",
         {Is(1, {TYPE_BYTES}), Is(2, {TYPE_STRING})}},
        {3, "bq_aead_encrypt($1, $2, $3)", {Is(1, {TYPE_BYTES}), Is(2, {TYPE_BYTES})}}}},
      {"DECRYPT_BYTES", {{3, "bq_aead_decrypt_bytes($1, $2, $3)", {Is(1, {TYPE_BYTES})}}}},
      {"DECRYPT_STRING",
       {{3, "bq_aead_decrypt_string($1, $2, encode($3))", {Is(1, {TYPE_BYTES})}}}},
      // Without max_distance, the distance is not capped.
      {"EDIT_DISTANCE",
       {{{2, 3}, "bq_edit_distance($1, $2, $3)", {Is(1, {TYPE_STRING})}, {"9223372036854775807"}},
        {{2, 3},
         "bq_edit_distance_bytes($1, $2, $3)",
         {Is(1, {TYPE_BYTES})},
         {"9223372036854775807"}}}},
      // Regular expressions. DuckDB raises no error for a replacement that refers to a missing
      // group, and returns '' or NULL elements where a capturing group takes no part in a match.
      {"REGEXP_CONTAINS", Strings("bq_regexp_contains", 2)},
      {"REGEXP_REPLACE", Strings("bq_regexp_replace", 3)},
      {"REGEXP_EXTRACT",
       {{{2, 4}, "bq_regexp_extract($1, $2, $3, $4)", {Is(1, {TYPE_STRING})}, {"1", "1"}},
        {{2, 4}, "bq_regexp_extract_bytes($1, $2, $3, $4)", {Is(1, {TYPE_BYTES})}, {"1", "1"}}}},
      {"REGEXP_EXTRACT_ALL", Strings("bq_regexp_extract_all", 2)},
      {"REGEXP_INSTR",
       {{{2, 5}, "bq_regexp_instr($1, $2, $3, $4, $5)", {Is(1, {TYPE_STRING})}, {"1", "1", "0"}},
        {{2, 5},
         "bq_regexp_instr_bytes($1, $2, $3, $4, $5)",
         {Is(1, {TYPE_BYTES})},
         {"1", "1", "0"}}}},
      // JSON goes to GoogleSQL as its text; see src/backend_functions.cc.
      {"PARSE_JSON", {{{1, 2}, "json(bq_parse_json($1, $2))", {}, {"'exact'"}}}},
      {"BOOL", {{1, "bq_json_bool(CAST($1 AS VARCHAR))", {Is(1, {TYPE_JSON})}}}},
      {"STRING",
       {{1, "bq_json_string(CAST($1 AS VARCHAR))", {Is(1, {TYPE_JSON})}},
        {{1, 2}, "bq_timestamp_string($1, $2)", {Is(1, {TYPE_TIMESTAMP})}, {"'UTC'"}}}},
      {"INT64", {{1, "bq_json_int64(CAST($1 AS VARCHAR))", {Is(1, {TYPE_JSON})}}}},
      {"FLOAT64",
       {{{1, 2}, "bq_json_float64(CAST($1 AS VARCHAR), $2)", {Is(1, {TYPE_JSON})}, {"'round'"}}}},
      {"DOUBLE",
       {{{1, 2}, "bq_json_float64(CAST($1 AS VARCHAR), $2)", {Is(1, {TYPE_JSON})}, {"'round'"}}}},
      {"JSON_TYPE", {{1, "bq_json_type(CAST($1 AS VARCHAR))"}}},
      {"$SUBSCRIPT",
       {{2,
         "json(bq_json_field(CAST($1 AS VARCHAR), $2))",
         {Is(1, {TYPE_JSON}), Is(2, {TYPE_STRING})}},
        {2,
         "json(bq_json_element(CAST($1 AS VARCHAR), $2))",
         {Is(1, {TYPE_JSON}), Is(2, {TYPE_INT64})}}}},
      {"JSON_QUERY", JsonExtract("bq_json_query", true, true)},
      {"JSON_EXTRACT", JsonExtract("bq_json_query", false, true)},
      {"JSON_VALUE", JsonExtract("bq_json_value", true, false)},
      {"JSON_EXTRACT_SCALAR", JsonExtract("bq_json_value", false, false)},
      {"JSON_QUERY_ARRAY", JsonExtract("bq_json_query_array", true, false)},
      {"JSON_EXTRACT_ARRAY", JsonExtract("bq_json_query_array", false, false)},
      {"JSON_VALUE_ARRAY", JsonExtract("bq_json_value_array", true, false)},
      {"JSON_EXTRACT_STRING_ARRAY", JsonExtract("bq_json_value_array", false, false)},
      {"LAX_BOOL", {{1, "bq_lax_bool(CAST($1 AS VARCHAR))", {Is(1, {TYPE_JSON})}}}},
      {"LAX_INT64", {{1, "bq_lax_int64(CAST($1 AS VARCHAR))", {Is(1, {TYPE_JSON})}}}},
      {"LAX_FLOAT64", {{1, "bq_lax_float64(CAST($1 AS VARCHAR))", {Is(1, {TYPE_JSON})}}}},
      {"LAX_DOUBLE", {{1, "bq_lax_float64(CAST($1 AS VARCHAR))", {Is(1, {TYPE_JSON})}}}},
      {"LAX_STRING", {{1, "bq_lax_string(CAST($1 AS VARCHAR))", {Is(1, {TYPE_JSON})}}}},
      {"JSON_KEYS",
       {{3,
         "CAST(json(bq_json_keys(CAST($1 AS VARCHAR), $2, $3)) AS VARCHAR[])",
         {Is(1, {TYPE_JSON})}}}},
      {"JSON_STRIP_NULLS",
       {{4, "json(bq_json_strip_nulls(CAST($1 AS VARCHAR), $2, $3, $4))", {Is(1, {TYPE_JSON})}}}},
  };
  return *kRules;
}

// BigQuery functions that DuckDB has under another name, with the same arguments.
const std::unordered_map<std::string_view, std::string_view>& FunctionNames() {
  static const auto* const kNames = new std::unordered_map<std::string_view, std::string_view>{
      {"GENERATE_UUID", "uuid"},
      {"IS_INF", "isinf"},
      {"IS_NAN", "isnan"},
      {"RAND", "random"},
      {"TIMESTAMP_SECONDS", "to_timestamp"},
      {"UNIX_MICROS", "epoch_us"},
  };
  return *kNames;
}

// BigQuery functions that DuckDB has under the same name, with the same arguments. Passing
// arbitrary builtin names through would accidentally accept internal functions and overloads
// with different semantics. Extend this list with execution coverage.
const std::unordered_set<std::string_view>& PlainFunctions() {
  static const auto* const kPlain = new std::unordered_set<std::string_view>{
      "IF",           "IFNULL", "NULLIF", "COALESCE", "CHAR_LENGTH", "CHARACTER_LENGTH",
      "ARRAY_LENGTH", "ATAN",   "ATAN2",  "TANH",     "ASINH"};
  return *kPlain;
}

// Functions that src/translator/function.cc translates in code.
const std::unordered_map<std::string_view, Handler>& Handlers() {
  static const auto* const kHandlers = new std::unordered_map<std::string_view, Handler>{
      {"$MAKE_ARRAY", MakeArray},
      {"$AND", Logical},
      {"$OR", Logical},
      {"$IN", InList},
      {"$CASE_NO_VALUE", Case},
      {"$CASE_WITH_VALUE", Case},
      {"DATE_BUCKET", Bucket},
      {"DATETIME_BUCKET", Bucket},
      {"TIMESTAMP_BUCKET", Bucket},
      {"TO_JSON", ToJson},
      {"TO_JSON_STRING", ToJson},
      {"JSON_ARRAY", JsonArray},
      {"JSON_REMOVE", JsonRemove},
      {"JSON_SET", JsonSet},
      {"JSON_OBJECT", JsonObject},
      {"FORMAT", Format},
      {"ARRAY_CONCAT", ArrayConcat},
      {"CONCAT", ConcatStrings},
      {"GREATEST", Extremum},
      {"LEAST", Extremum},
  };
  return *kHandlers;
}

// SAFE_ADD and its siblings are the arithmetic operators with the SAFE. prefix.
const std::unordered_map<std::string_view, std::string_view>& SafeFunctions() {
  static const auto* const kSafe = new std::unordered_map<std::string_view, std::string_view>{
      {"SAFE_ADD", "$ADD"},
      {"SAFE_SUBTRACT", "$SUBTRACT"},
      {"SAFE_MULTIPLY", "$MULTIPLY"},
      {"SAFE_NEGATE", "$UNARY_MINUS"},
  };
  return *kSafe;
}

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
      .arguments = {arity == 1 ? "CAST($1 AS VARCHAR)"
                               : "CAST($1 AS VARCHAR) || ',' || CAST($2 AS VARCHAR)"},
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
const std::unordered_map<std::string_view, std::vector<AggregateRule>>& Aggregates() {
  using enum AggregateRule::Nulls;
  using enum AggregateRule::Limit;
  static const auto* const kAggregates =
      new std::unordered_map<std::string_view, std::vector<AggregateRule>>{
          {"COUNT", {{.function = "count"}}},
          {"$COUNT_STAR", {{.function = "count", .arguments = {"*"}}}},
          {"SUM",
           {{.function = "sum", .type = googlesql::TYPE_BIGNUMERIC, .finish = BigNumericSum},
            {.function = "sum"}}},
          {"AVG", {BigNumericAggregate<kBigNumericAvg>(1), {.function = "avg"}}},
          {"MIN", {{.function = "min"}}},
          {"MAX", {{.function = "max"}}},
          {"ANY_VALUE", {{.function = "any_value"}}},
          {"ARRAY_AGG", {{.function = "list", .nulls = kFilter, .limit = kSlice}}},
          // ARRAY_CONCAT_AGG skips NULL arrays.
          {"ARRAY_CONCAT_AGG",
           {{.function = "list", .limit = kSlice, .skip_nulls = true, .finish = Flatten}}},
          // Exact, which is within any approximation error.
          {"APPROX_COUNT_DISTINCT",
           {{.function = "count", .distinct = AggregateRule::Distinct::kAlways}}},
          // APPROX_QUANTILES(x, n) takes the n + 1 quantiles 0, 1/n, ..., 1. quantile_disc()
          // skips NULLs, as APPROX_QUANTILES does by default.
          {"APPROX_QUANTILES",
           {{.function = "quantile_disc",
             .arguments = {"$1", "list_transform(range($2 + 1), lambda i: i / $2)"},
             .nulls = kIgnore}}},
          // histogram() takes only the values; TopCount() applies the count afterwards.
          {"APPROX_TOP_COUNT",
           {{.function = "histogram",
             .arguments = {"$1"},
             .distinct = AggregateRule::Distinct::kUnsupported,
             .finish = TopCount}}},
          // The _null variants return a NULL x instead of skipping its row.
          {"MAX_BY", {{.function = "arg_max_null"}}},
          {"MIN_BY", {{.function = "arg_min_null"}}},
          // DuckDB's string_agg() only joins strings, so STRING_AGG over BYTES joins their
          // hexadecimal digits; the default delimiter is b','.
          {"STRING_AGG",
           {{.function = "string_agg", .type = googlesql::TYPE_STRING, .limit = kJoin},
            {.function = "string_agg",
             .type = googlesql::TYPE_BYTES,
             .arguments = {"hex($1)", "hex($2)"},
             .defaults = {"", "unhex('2C')"},
             .limit = kJoin,
             .finish = Unhex}}},
          {"COUNTIF", {{.function = "count_if"}}},
          {"LOGICAL_AND", {{.function = "bool_and"}}},
          {"LOGICAL_OR", {{.function = "bool_or"}}},
          {"BIT_AND", {{.function = "bit_and"}}},
          {"BIT_OR", {{.function = "bit_or"}}},
          {"BIT_XOR", {{.function = "bit_xor"}}},
          {"STDDEV", {BigNumericAggregate<kBigNumericStdDevSamp>(1), {.function = "stddev_samp"}}},
          {"STDDEV_SAMP",
           {BigNumericAggregate<kBigNumericStdDevSamp>(1), {.function = "stddev_samp"}}},
          {"STDDEV_POP",
           {BigNumericAggregate<kBigNumericStdDevPop>(1), {.function = "stddev_pop"}}},
          {"VARIANCE", {BigNumericAggregate<kBigNumericVarSamp>(1), {.function = "var_samp"}}},
          {"VAR_SAMP", {BigNumericAggregate<kBigNumericVarSamp>(1), {.function = "var_samp"}}},
          {"VAR_POP", {BigNumericAggregate<kBigNumericVarPop>(1), {.function = "var_pop"}}},
          {"CORR", {BigNumericAggregate<kBigNumericCorr>(2), {.function = "corr"}}},
          {"COVAR_POP", {BigNumericAggregate<kBigNumericCovarPop>(2), {.function = "covar_pop"}}},
          {"COVAR_SAMP",
           {BigNumericAggregate<kBigNumericCovarSamp>(2), {.function = "covar_samp"}}},
      };
  return *kAggregates;
}

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
// FLOAT64 or, with `kNumeric`, a NUMERIC percentile.
template <bool kNumeric>
std::string PercentileDisc(const std::string& sql, const std::vector<std::string>& arguments,
                           const std::string& /*tail*/) {
  return "list_extract(" + sql + ", " +
         (kNumeric ? "bq_percentile_disc_position_numeric(len("
                   : "bq_percentile_disc_position(len(") +
         sql + "), " + arguments.at(1) + "))";
}

// PERCENTILE_DISC of values of `type`, for a percentile of `percentile`.
AggregateRule PercentileDiscRule(googlesql::TypeKind type, googlesql::TypeKind percentile) {
  const bool numeric = percentile == googlesql::TYPE_NUMERIC;
  return {
      .function = "list",
      .type = type,
      .conditions = {{.argument = 2, .types = {percentile}}},
      .arguments = {"$1"},
      .order = type == googlesql::TYPE_DOUBLE ? kFloatPercentileOrder : kPercentileOrder,
      .nulls = AggregateRule::Nulls::kFilterUnlessRespected,
      .finish = numeric ? PercentileDisc<true> : PercentileDisc<false>,
  };
}

// Functions that only take a window.
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
          {"PERCENTILE_CONT",
           {{.function = "list",
             .type = googlesql::TYPE_DOUBLE,
             .arguments = {"$1"},
             .order = kFloatPercentileOrder,
             .nulls = kFilterUnlessRespected,
             .finish = PercentileCont<false>},
            {.function = "list",
             .type = googlesql::TYPE_NUMERIC,
             .arguments = {"$1"},
             .order = kPercentileOrder,
             .nulls = kFilterUnlessRespected,
             .finish = PercentileCont<true>}}},
          {"PERCENTILE_DISC",
           {PercentileDiscRule(googlesql::TYPE_DOUBLE, googlesql::TYPE_DOUBLE),
            PercentileDiscRule(googlesql::TYPE_DOUBLE, googlesql::TYPE_NUMERIC),
            PercentileDiscRule(googlesql::TYPE_UNKNOWN, googlesql::TYPE_DOUBLE),
            PercentileDiscRule(googlesql::TYPE_UNKNOWN, googlesql::TYPE_NUMERIC)}},
      };
  return *kAnalytics;
}

// The functions that handle BIGNUMERIC, by carrying it as it is, by comparing BIGNUM, which
// DuckDB does exactly, or by rules of their own.
const std::unordered_set<std::string_view>& BigNumericFunctions() {
  static const auto* const kFunctions = new std::unordered_set<std::string_view>{
      // Comparisons.
      "$EQUAL",
      "$NOT_EQUAL",
      "$LESS",
      "$LESS_OR_EQUAL",
      "$GREATER",
      "$GREATER_OR_EQUAL",
      "$BETWEEN",
      "$IS_DISTINCT_FROM",
      "$IS_NOT_DISTINCT_FROM",
      "$IS_NULL",
      "$IN",
      "$IN_ARRAY",
      "GREATEST",
      "LEAST",
      "RANGE_BUCKET",
      // Carrying values.
      "$CASE_NO_VALUE",
      "$CASE_WITH_VALUE",
      "IF",
      "IFNULL",
      "COALESCE",
      "NULLIF",
      // ERROR returns no value; GoogleSQL calls it with the BIGNUMERIC type of ARRAY_FIRST and
      // ARRAY_LAST.
      "ERROR",
      "$MAKE_ARRAY",
      "$ARRAY_AT_OFFSET",
      "$ARRAY_AT_ORDINAL",
      "$SAFE_ARRAY_AT_OFFSET",
      "$SAFE_ARRAY_AT_ORDINAL",
      // Arithmetic.
      "$ADD",
      "$SUBTRACT",
      "$UNARY_MINUS",
      "$MULTIPLY",
      "$DIVIDE",
      "SAFE_DIVIDE",
      "DIV",
      "MOD",
      "ABS",
      "SIGN",
      "ROUND",
      "TRUNC",
      "CEIL",
      "CEILING",
      "FLOOR",
      "SQRT",
      "CBRT",
      "POW",
      "POWER",
      "EXP",
      "LN",
      "LOG",
      "LOG10",
      // Arrays, JSON and text.
      "ARRAY_LENGTH",
      "ARRAY_REVERSE",
      "ARRAY_CONCAT",
      "GENERATE_ARRAY",
      "TO_JSON",
      "TO_JSON_STRING",
      "JSON_ARRAY",
      "JSON_OBJECT",
      "JSON_SET",
      "FORMAT",
      "PARSE_BIGNUMERIC",
      // Aggregate and analytic functions.
      "COUNT",
      "MIN",
      "MAX",
      "ANY_VALUE",
      "ARRAY_AGG",
      "ARRAY_CONCAT_AGG",
      "MAX_BY",
      "MIN_BY",
      "APPROX_COUNT_DISTINCT",
      "APPROX_QUANTILES",
      "APPROX_TOP_COUNT",
      "SUM",
      "AVG",
      "STDDEV",
      "STDDEV_SAMP",
      "STDDEV_POP",
      "VARIANCE",
      "VAR_SAMP",
      "VAR_POP",
      "CORR",
      "COVAR_POP",
      "COVAR_SAMP",
      "LAG",
      "LEAD",
      "FIRST_VALUE",
      "LAST_VALUE",
      "NTH_VALUE",
  };
  return *kFunctions;
}

// Every function, from the tables above. A function is in one table only.
const std::unordered_map<std::string_view, FunctionEntry>& Registry() {
  static const auto* const kRegistry = [] {
    auto* registry = new std::unordered_map<std::string_view, FunctionEntry>();
    const auto add = [registry](std::string_view name, FunctionEntry entry) {
      if (!registry->emplace(name, std::move(entry)).second) {
        std::cerr << "function " << name << " is registered twice\n";
        std::abort();
      }
    };
    for (const auto& [name, rules] : TemplateRules()) {
      add(name, {.implementation = Implementation::kRules, .rules = rules});
    }
    for (const auto& [name, rules] : BackendRules()) {
      add(name, {.implementation = Implementation::kBackend, .rules = rules});
    }
    for (const auto& [name, target] : FunctionNames()) {
      add(name, {.implementation = Implementation::kRenamed, .target = target});
    }
    for (const std::string_view name : PlainFunctions()) {
      add(name, {.implementation = Implementation::kSame});
    }
    for (const auto& [name, handler] : Handlers()) {
      add(name, {.implementation = Implementation::kHandler, .handler = handler});
    }
    for (const auto& [name, target] : SafeFunctions()) {
      add(name, {.implementation = Implementation::kSafe, .target = target});
    }
    for (const auto& [name, rules] : Aggregates()) {
      add(name, {.implementation = Implementation::kAggregate, .aggregates = rules});
    }
    for (const auto& [name, rules] : Analytics()) {
      add(name, {.implementation = Implementation::kAnalytic, .aggregates = rules});
    }
    return registry;
  }();
  return *kRegistry;
}

}  // namespace

std::string_view Describe(Implementation implementation) {
  switch (implementation) {
    case Implementation::kSame:
      return "DuckDB function";
    case Implementation::kRenamed:
      return "DuckDB function, renamed";
    case Implementation::kRules:
      return "DuckDB SQL";
    case Implementation::kBackend:
      return "GoogleSQL function";
    case Implementation::kHandler:
      return "DuckDB SQL, in code";
    case Implementation::kSafe:
      return "SAFE. operator";
    case Implementation::kAggregate:
      return "DuckDB aggregate";
    case Implementation::kAnalytic:
      return "DuckDB window function";
  }
  return "";
}

bool SupportsBigNumeric(std::string_view upper_name) {
  return BigNumericFunctions().contains(upper_name);
}

const FunctionEntry* FindFunction(std::string_view upper_name) {
  const auto entry = Registry().find(upper_name);
  return entry == Registry().end() ? nullptr : &entry->second;
}

}  // namespace bigquery_emulator_duckdb::translator
