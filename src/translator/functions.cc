#include "src/translator/functions.h"

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <initializer_list>
#include <iostream>
#include <optional>
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

Condition Literal(std::size_t argument, std::string_view value) {
  return {.argument = argument, .string_literal = value};
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

// The hexadecimal digits of the BYTES value `value` with a space after each byte, as in
// "FF 61 ". A search for one such string in another only matches on byte boundaries.
std::string Spaced(std::string_view value) {
  return "regexp_replace(hex(" + std::string(value) + "), '(..)', '\\1 ', 'g')";
}

// The BYTES value `value` as a string of one character per byte, U+0100 plus the byte, for the
// string functions to work on. FromChars() turns such a string back into BYTES.
std::string ToChars(std::string_view value) {
  return "array_to_string(list_transform(regexp_extract_all(hex(" + std::string(value) +
         "), '..'), _b -> chr(256 + CAST('0x' || _b AS INTEGER))), '')";
}

std::string FromChars(std::string_view chars) {
  return "unhex(array_to_string(list_transform(regexp_extract_all(" + std::string(chars) +
         ", '.'), _c -> lpad(hex(unicode(_c) - 256), 2, '0')), ''))";
}

// STRPOS and INSTR without a position or occurrence. A match in the spaced digits starts on a
// byte, three characters each.
std::vector<Rule> Position() {
  return {{2, "strpos($1, $2)", {Is(1, {TYPE_STRING})}},
          {2,
           "((strpos(" + Spaced("$1") + ", " + Spaced("$2") + ") + 2) // 3)",
           {Is(1, {TYPE_BYTES})}}};
}

// LEFT and RIGHT.
std::vector<Rule> Side(const std::string& function, const std::string& name) {
  const std::vector<std::string> errors = {"'" + name + " length must be non-negative'"};
  return {{2,
           "CASE WHEN $2 < 0 THEN !1 ELSE " + function + "($1, $2) END",
           {Is(1, {TYPE_STRING})},
           {},
           errors},
          {2,
           "CASE WHEN $2 < 0 THEN !1 ELSE unhex(" + function + "(hex($1), 2 * $2)) END",
           {Is(1, {TYPE_BYTES})},
           {},
           errors}};
}

// LPAD and RPAD. DuckDB has no default pad; the BYTES one is b' '.
std::vector<Rule> Pad(const std::string& function) {
  return {
      {{2, 3}, function + "($1, CAST($2 AS INTEGER), $3)", {Is(1, {TYPE_STRING})}, {"' '"}},
      {2, "unhex(" + function + "(hex($1), CAST(2 * $2 AS INTEGER), '20'))", {Is(1, {TYPE_BYTES})}},
      {3,
       "unhex(" + function + "(hex($1), CAST(2 * $2 AS INTEGER), hex($3)))",
       {Is(1, {TYPE_BYTES})}}};
}

// TRIM, LTRIM and RTRIM. For BYTES, the regular expression `pattern` removes the bytes to trim
// from the spaced digits, with @ standing for the alternatives of the bytes of the second
// argument.
std::vector<Rule> Trim(const std::string& function, std::string_view pattern) {
  const std::string bytes = "' || substr(regexp_replace(hex($2), '(..)', '|\\1 ', 'g'), 2) || '";
  std::string regex;
  for (const char c : pattern) {
    regex += c == '@' ? bytes : std::string(1, c);
  }
  auto rules = Same(function, {1, 2}, {Is(1, {TYPE_STRING})});
  rules.push_back(
      {2,
       "unhex(replace(regexp_replace(" + Spaced("$1") + ", '" + regex + "', '', 'g'), ' ', ''))",
       {Is(1, {TYPE_BYTES})}});
  return rules;
}

// SUBSTR and SUBSTRING. BigQuery starts at the first character for a position of 0 or one
// before the start, where DuckDB takes as many characters fewer.
std::vector<Rule> Substr() {
  const auto start = [](const std::string& length) {
    return "CASE WHEN $2 > 0 THEN $2 WHEN $2 = 0 OR $2 < -" + length + " THEN 1 ELSE " + length +
           " + $2 + 1 END";
  };
  const std::string negative = "CASE WHEN $3 < 0 THEN !1 ELSE ";
  const std::vector<std::string> errors = {"'Third argument in SUBSTR() cannot be negative'"};
  return {
      {2, "substr($1, " + start("length($1)") + ")", {Is(1, {TYPE_STRING})}},
      {3,
       negative + "substr($1, " + start("length($1)") + ", $3) END",
       {Is(1, {TYPE_STRING})},
       {},
       errors},
      {2,
       "unhex(substr(hex($1), 2 * (" + start("octet_length($1)") + ") - 1))",
       {Is(1, {TYPE_BYTES})}},
      {3,
       negative + "unhex(substr(hex($1), 2 * (" + start("octet_length($1)") + ") - 1, 2 * $3)) END",
       {Is(1, {TYPE_BYTES})},
       {},
       errors}};
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

// The reciprocal of a DuckDB function, which BigQuery reports as an error at zero where DuckDB
// returns infinity.
std::vector<Rule> Reciprocal(std::string_view function) {
  const std::string value = std::string(function) + "($1)";
  return {{1,
           "CASE WHEN " + value + " = 0 THEN !1 ELSE 1 / " + value + " END",
           {},
           {},
           {"'Floating point error: division by zero'"}}};
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

// Conversions from JSON fail unless the value has the requested type; SQL NULL stays NULL.
// FLOAT64's wide_number_mode 'exact' fails on a loss of precision, which is unsupported.
std::vector<Rule> FromJson(std::string_view when, std::string_view expected) {
  return {{{1, 2},
           "list_transform([$1], _j -> CASE WHEN _j IS NULL THEN NULL WHEN " + std::string(when) +
               " ELSE !1 END)[1]",
           {Is(1, {TYPE_JSON}), Literal(2, "round")},
           {},
           {"'The provided JSON input is not " + std::string(expected) + "'"}}};
}

std::vector<Rule> Concat(std::initializer_list<std::vector<Rule>> groups) {
  std::vector<Rule> rules;
  for (const auto& group : groups) {
    rules.insert(rules.end(), group.begin(), group.end());
  }
  return rules;
}

// Functions implemented by DuckDB SQL templates.
const std::unordered_map<std::string_view, std::vector<Rule>>& TemplateRules() {
  static const auto* const kRules = new std::unordered_map<std::string_view, std::vector<Rule>>{
      // Operators. Division binds its operands once, as every rule does, and stays an
      // expression that CASE and IF can short-circuit; DuckDB would return infinity on zero.
      {"$ADD", {{2, "($1 + $2)"}}},
      {"$SUBTRACT", {{2, "($1 - $2)"}}},
      {"$MULTIPLY", {{2, "($1 * $2)"}}},
      {"$DIVIDE",
       {{2,
         "CASE WHEN $1 IS NULL OR $2 IS NULL THEN NULL WHEN $2 = 0 THEN !1 ELSE $1 / $2 END",
         {},
         {},
         {"'division by zero'"}}}},
      {"$UNARY_MINUS", {{1, "(-$1)"}}},
      {"$EQUAL", {{2, "($1 = $2)"}}},
      {"$NOT_EQUAL", {{2, "($1 <> $2)"}}},
      {"$LESS", {{2, "($1 < $2)"}}},
      {"$LESS_OR_EQUAL", {{2, "($1 <= $2)"}}},
      {"$GREATER", {{2, "($1 > $2)"}}},
      {"$GREATER_OR_EQUAL", {{2, "($1 >= $2)"}}},
      {"$BETWEEN", {{3, "($1 BETWEEN $2 AND $3)"}}},
      {"$LIKE", {{2, "($1 LIKE $2)"}}},
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
      {"SAFE_DIVIDE", {{2, "($1 / NULLIF($2, 0))"}}},
      {"IEEE_DIVIDE", {{2, "(CAST($1 AS DOUBLE) / CAST($2 AS DOUBLE))"}}},
      // DuckDB's log() is the base 10 logarithm, BigQuery's LOG() the natural one, and the two
      // argument form takes the base first rather than last.
      {"LOG", {{1, "ln($1)"}, {2, "log($2, $1)"}}},
      // DuckDB takes the digits as an INTEGER.
      // DuckDB rounds halfway values away from zero, and its round_even() goes through
      // DOUBLE, so ROUND_HALF_EVEN takes the truncated value instead at a tie whose truncated
      // value is even. Only NUMERIC and BIGNUMERIC take a rounding mode.
      {"ROUND",
       {{1, "round($1)"},
        {2, "round($1, CAST($2 AS INTEGER))"},
        {3, "round($1, CAST($2 AS INTEGER))", {Mode(3, "ROUND_HALF_AWAY_FROM_ZERO")}},
        {3, RoundHalfEven(), {Mode(3, "ROUND_HALF_EVEN")}}}},
      {"TRUNC", {{1, "trunc($1)"}, {2, "trunc($1, CAST($2 AS INTEGER))"}}},
      {"SEC", Reciprocal("cos")},
      {"CSC", Reciprocal("sin")},
      {"SECH", Reciprocal("cosh")},
      {"CSCH", Reciprocal("sinh")},
      {"COTH", Reciprocal("tanh")},
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
      {"TIMESTAMP",
       {{1, "CAST($1 AS TIMESTAMPTZ)"},
        // A civil time in the given zone; a string with its own offset keeps the offset.
        {2, "timezone($2, CAST($1 AS TIMESTAMP))", {Is(1, {TYPE_DATE, TYPE_DATETIME})}}}},

      // Formatting and parsing: DuckDB takes the value first and the format second.
      {"FORMAT_DATE", {{2, "strftime($2, $1)"}}},
      {"FORMAT_DATETIME", {{2, "strftime($2, $1)"}}},
      {"FORMAT_TIMESTAMP", {{2, "strftime($2, $1)"}}},
      // DuckDB's strftime() has no TIME overload.
      {"FORMAT_TIME", {{2, "strftime(DATE '1970-01-01' + $2, $1)"}}},
      {"PARSE_DATE", {{2, "CAST(strptime($2, $1) AS DATE)"}}},
      {"PARSE_DATETIME", {{2, "strptime($2, $1)"}}},
      {"PARSE_TIME", {{2, "CAST(strptime($2, $1) AS TIME)"}}},
      {"PARSE_TIMESTAMP", {{2, "CAST(strptime($2, $1) AS TIMESTAMPTZ)"}}},

      // Epoch conversions. The DuckDB functions return a civil timestamp, which is read as UTC
      // to arrive at the instant BigQuery means.
      {"UNIX_SECONDS", {{1, "CAST(epoch($1) AS BIGINT)"}}},
      {"UNIX_DATE", {{1, "date_diff('day', DATE '1970-01-01', $1)"}}},
      {"TIMESTAMP_MILLIS", {{1, "(epoch_ms($1) AT TIME ZONE 'UTC')"}}},
      {"TIMESTAMP_MICROS", {{1, "(make_timestamp($1) AT TIME ZONE 'UTC')"}}},
      {"DATE_FROM_UNIX_DATE",
       {{1, "CAST(DATE '1970-01-01' + to_days(CAST($1 AS INTEGER)) AS DATE)"}}},

      // Strings. DuckDB has almost no BLOB functions, so the BYTES rules work on the hexadecimal
      // digits, two to a byte; see Spaced().
      {"LENGTH", {{1, "octet_length($1)", {Is(1, {TYPE_BYTES})}}, {1, "length($1)"}}},
      {"BYTE_LENGTH",
       {{1, "strlen($1)", {Is(1, {TYPE_STRING})}}, {1, "octet_length($1)", {Is(1, {TYPE_BYTES})}}}},
      {"INSTR", Position()},
      {"STRPOS", Position()},
      {"LEFT", Side("left", "LEFT")},
      {"RIGHT", Side("right", "RIGHT")},
      {"LPAD", Pad("lpad")},
      {"RPAD", Pad("rpad")},
      {"SPLIT",
       {{{1, 2}, "split($1, $2)", {Is(1, {TYPE_STRING})}, {"','"}},
        // An empty delimiter splits the value into its bytes.
        {2,
         "list_transform(CASE WHEN octet_length($2) = 0 THEN regexp_extract_all(hex($1), '..') "
         "ELSE list_transform(string_split(" +
             Spaced("$1") + ", " + Spaced("$2") +
             "), _p -> replace(_p, ' ', '')) END, _p -> unhex(_p))",
         {Is(1, {TYPE_BYTES})}}}},
      {"TRANSLATE",
       {{3, "translate($1, $2, $3)", {Is(1, {TYPE_STRING})}},
        {3,
         FromChars("translate(" + ToChars("$1") + ", " + ToChars("$2") + ", " + ToChars("$3") +
                   ")"),
         {Is(1, {TYPE_BYTES})}}}},
      {"ASCII",
       {{1, "ascii($1)", {Is(1, {TYPE_STRING})}},
        {1,
         "CASE WHEN octet_length($1) = 0 THEN 0 ELSE CAST('0x' || left(hex($1), 2) AS BIGINT) END",
         {Is(1, {TYPE_BYTES})}}}},
      {"UNICODE", {{1, "CASE WHEN $1 = '' THEN 0 ELSE unicode($1) END", {Is(1, {TYPE_STRING})}}}},
      {"CHR", {{1, "CASE WHEN $1 = 0 THEN '' ELSE chr(CAST($1 AS INTEGER)) END"}}},
      {"NORMALIZE", {{1, "nfc_normalize($1)", {Is(1, {TYPE_STRING})}}}},
      // REGEXP_REPLACE replaces every occurrence; DuckDB needs the global flag for that.
      {"REGEXP_REPLACE", {{3, "regexp_replace($1, $2, $3, 'g')", {Is(1, {TYPE_STRING})}}}},
      {"REGEXP_CONTAINS", {{2, "regexp_matches($1, $2)", {Is(1, {TYPE_STRING})}}}},
      // A BLOB cast to VARCHAR escapes every byte but printable ASCII as \xHH, so the case
      // mapping only touches ASCII letters, and the escapes read back in either case but \X.
      {"LOWER",
       {{1, "lower($1)", {Is(1, {TYPE_STRING})}},
        {1, "CAST(lower(CAST($1 AS VARCHAR)) AS BLOB)", {Is(1, {TYPE_BYTES})}}}},
      {"UPPER",
       {{1, "upper($1)", {Is(1, {TYPE_STRING})}},
        {1,
         "CAST(replace(upper(CAST($1 AS VARCHAR)), '\\X', '\\x') AS BLOB)",
         {Is(1, {TYPE_BYTES})}}}},
      // Reversing the digits reverses the bytes and swaps the two digits of each.
      {"REVERSE",
       {{1, "reverse($1)", {Is(1, {TYPE_STRING})}},
        {1,
         "unhex(regexp_replace(reverse(hex($1)), '(.)(.)', '\\2\\1', 'g'))",
         {Is(1, {TYPE_BYTES})}}}},
      {"TRIM", Trim("trim", "^(?:@)+|(?:@)+$")},
      {"LTRIM", Trim("ltrim", "^(?:@)+")},
      {"RTRIM", Trim("rtrim", "(?:@)+$")},
      {"SUBSTR", Substr()},
      {"SUBSTRING", Substr()},
      {"STARTS_WITH",
       {{2, "starts_with($1, $2)", {Is(1, {TYPE_STRING})}},
        {2, "starts_with(hex($1), hex($2))", {Is(1, {TYPE_BYTES})}}}},
      {"ENDS_WITH",
       {{2, "ends_with($1, $2)", {Is(1, {TYPE_STRING})}},
        {2, "ends_with(hex($1), hex($2))", {Is(1, {TYPE_BYTES})}}}},
      {"REPLACE",
       {{3, "replace($1, $2, $3)", {Is(1, {TYPE_STRING})}},
        {3,
         "unhex(replace(replace(" + Spaced("$1") + ", " + Spaced("$2") + ", " + Spaced("$3") +
             "), ' ', ''))",
         {Is(1, {TYPE_BYTES})}}}},

      // Hashes are BYTES in BigQuery and hexadecimal strings in DuckDB.
      {"MD5", {{1, "unhex(md5($1))"}}},
      {"SHA1", {{1, "unhex(sha1($1))"}}},
      {"SHA256", {{1, "unhex(sha256($1))"}}},
      // IPv4 addresses are 4 bytes in network byte order. A negative integer stands for its
      // 32-bit two's complement, which the mask makes non-negative for hex() to print.
      {"IPV4_FROM_INT64",
       {{1,
         "CASE WHEN $1 < -2147483648 OR $1 > 4294967295 THEN !1 ELSE unhex(lpad(hex($1 & "
         "4294967295), 8, '0')) END",
         {Is(1, {TYPE_INT64})},
         {},
         {"'NET.IPV4_FROM_INT64() encountered an invalid integer IP. Expected range: "
          "[-0x80000000, 0xFFFFFFFF]; got ' || $1"}}}},
      {"IPV4_TO_INT64",
       {{1,
         "CASE WHEN octet_length($1) <> 4 THEN !1 ELSE CAST('0x' || hex($1) AS BIGINT) END",
         {Is(1, {TYPE_BYTES})},
         {},
         {"'NET.IPV4_TO_INT64() encountered a non-IPv4 address. Expected 4 bytes but got ' || "
          "octet_length($1)"}}}},
      {"TO_HEX", {{1, "lower(hex($1))"}}},
      {"FROM_HEX", {{1, "unhex($1)", {Is(1, {TYPE_STRING})}}}},
      {"TO_BASE64", {{1, "to_base64($1)"}}},
      {"FROM_BASE64", {{1, "from_base64($1)", {Is(1, {TYPE_STRING})}}}},

      // JSON. Only the exact wide number mode keeps DuckDB's numbers as they are.
      {"PARSE_JSON", {{{1, 2}, "json($1)", {Literal(2, "exact")}}}},
      {"TO_JSON_STRING", {{1, "CAST(to_json($1) AS VARCHAR)"}}},
      {"BOOL", FromJson("json_type(_j) = 'BOOLEAN' THEN CAST(_j AS BOOLEAN)", "a boolean")},
      {"STRING",
       FromJson("json_type(_j) = 'VARCHAR' THEN json_extract_string(_j, '$')", "a string")},
      {"INT64",
       FromJson("json_type(_j) IN ('BIGINT', 'UBIGINT') THEN CAST(_j AS BIGINT) WHEN "
                "json_type(_j) = 'DOUBLE' AND CAST(_j AS DOUBLE) = trunc(CAST(_j AS DOUBLE)) THEN "
                "CAST(CAST(_j AS DOUBLE) AS BIGINT)",
                "an integer")},
      {"FLOAT64",
       FromJson("json_type(_j) IN ('BIGINT', 'UBIGINT', 'DOUBLE') THEN CAST(_j AS DOUBLE)",
                "a number")},
      {"DOUBLE",
       FromJson("json_type(_j) IN ('BIGINT', 'UBIGINT', 'DOUBLE') THEN CAST(_j AS DOUBLE)",
                "a number")},
      {"JSON_TYPE",
       {{1,
         "CASE json_type($1) WHEN 'OBJECT' THEN 'object' WHEN 'ARRAY' THEN 'array' "
         "WHEN 'VARCHAR' THEN 'string' WHEN 'BOOLEAN' THEN 'boolean' WHEN 'NULL' THEN 'null' "
         "WHEN 'BIGINT' THEN 'number' WHEN 'UBIGINT' THEN 'number' "
         "WHEN 'DOUBLE' THEN 'number' END"}}},

      {"ERROR", {{1, "!1", {}, {}, {"$1"}}}},
      {"ARRAY_REVERSE", {{1, "list_reverse($1)"}}},
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
      // step, which BigQuery rejects.
      {"GENERATE_ARRAY",
       {{{2, 3},
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
      // encode() takes a STRING's UTF-8 bytes.
      {"SHA512",
       {{1, "bq_sha512(encode($1))", {Is(1, {TYPE_STRING})}},
        {1, "bq_sha512($1)", {Is(1, {TYPE_BYTES})}}}},
      {"FARM_FINGERPRINT",
       {{1, "bq_farm_fingerprint(encode($1))", {Is(1, {TYPE_STRING})}},
        {1, "bq_farm_fingerprint($1)", {Is(1, {TYPE_BYTES})}}}},
      {"INITCAP", {{1, "bq_initcap($1)"}, {2, "bq_initcap_delimiters($1, $2)"}}},
      // Without max_distance, the distance is not capped.
      {"EDIT_DISTANCE",
       {{{2, 3}, "bq_edit_distance($1, $2, $3)", {Is(1, {TYPE_STRING})}, {"9223372036854775807"}},
        {{2, 3},
         "bq_edit_distance_bytes($1, $2, $3)",
         {Is(1, {TYPE_BYTES})},
         {"9223372036854775807"}}}},
      {"REGEXP_INSTR",
       {{{2, 5}, "bq_regexp_instr($1, $2, $3, $4, $5)", {Is(1, {TYPE_STRING})}, {"1", "1", "0"}},
        {{2, 5},
         "bq_regexp_instr_bytes($1, $2, $3, $4, $5)",
         {Is(1, {TYPE_BYTES})},
         {"1", "1", "0"}}}},
      // These take and return JSON as its text; see src/backend_functions.cc.
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
      {"CONTAINS_SUBSTR", "contains"},
      {"DIV", "divide"},
      {"FORMAT", "printf"},
      {"GENERATE_UUID", "uuid"},
      {"IS_INF", "isinf"},
      {"IS_NAN", "isnan"},
      {"JSON_ARRAY", "json_array"},
      {"RAND", "random"},
      {"TIMESTAMP_SECONDS", "to_timestamp"},
      {"UNIX_MICROS", "epoch_us"},
      {"UNIX_MILLIS", "epoch_ms"},
  };
  return *kNames;
}

// BigQuery functions that DuckDB has under the same name, with the same arguments. Passing
// arbitrary builtin names through would accidentally accept internal functions and overloads
// with different semantics. Extend this list with execution coverage.
const std::unordered_set<std::string_view>& PlainFunctions() {
  static const auto* const kPlain = new std::unordered_set<std::string_view>{
      "ABS",    "SIGN",   "CEIL",         "CEILING",     "FLOOR",
      "SQRT",   "POW",    "POWER",        "EXP",         "LN",
      "LOG10",  "MOD",    "GREATEST",     "LEAST",       "IF",
      "IFNULL", "NULLIF", "COALESCE",     "CHAR_LENGTH", "CHARACTER_LENGTH",
      "CONCAT", "REPEAT", "ARRAY_LENGTH", "SIN",         "COS",
      "TAN",    "ASIN",   "ACOS",         "ATAN",        "ATAN2",
      "TANH",   "ASINH",  "CBRT",         "ACOSH",       "ATANH",
      "SINH",   "COSH",   "COT"};
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
      {"$SUBSCRIPT", JsonSubscript},
      {"DATE_BUCKET", Bucket},
      {"DATETIME_BUCKET", Bucket},
      {"TIMESTAMP_BUCKET", Bucket},
      {"REGEXP_EXTRACT", RegexpExtract},
      {"REGEXP_EXTRACT_ALL", RegexpExtract},
      {"JSON_QUERY", JsonExtract},
      {"JSON_EXTRACT", JsonExtract},
      {"JSON_VALUE", JsonExtract},
      {"JSON_EXTRACT_SCALAR", JsonExtract},
      {"JSON_QUERY_ARRAY", JsonExtract},
      {"JSON_EXTRACT_ARRAY", JsonExtract},
      {"JSON_VALUE_ARRAY", JsonExtract},
      {"JSON_EXTRACT_STRING_ARRAY", JsonExtract},
      {"TO_JSON", ToJson},
      {"JSON_REMOVE", JsonRemove},
      {"JSON_SET", JsonSet},
      {"JSON_OBJECT", JsonObject},
      {"ARRAY_CONCAT", ArrayConcat},
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

// Aggregate functions, which also take a window.
const std::unordered_map<std::string_view, std::vector<AggregateRule>>& Aggregates() {
  using enum AggregateRule::Nulls;
  using enum AggregateRule::Limit;
  static const auto* const kAggregates =
      new std::unordered_map<std::string_view, std::vector<AggregateRule>>{
          {"COUNT", {{.function = "count"}}},
          {"$COUNT_STAR", {{.function = "count", .arguments = {"*"}}}},
          {"SUM", {{.function = "sum"}}},
          {"AVG", {{.function = "avg"}}},
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
          {"STDDEV", {{.function = "stddev_samp"}}},
          {"STDDEV_SAMP", {{.function = "stddev_samp"}}},
          {"STDDEV_POP", {{.function = "stddev_pop"}}},
          {"VARIANCE", {{.function = "var_samp"}}},
          {"VAR_SAMP", {{.function = "var_samp"}}},
          {"VAR_POP", {{.function = "var_pop"}}},
          {"CORR", {{.function = "corr"}}},
          {"COVAR_POP", {{.function = "covar_pop"}}},
          {"COVAR_SAMP", {{.function = "covar_samp"}}},
      };
  return *kAggregates;
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
      };
  return *kAnalytics;
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

// The argument a $n, #n or !n at spelling[i] refers to, counted from 0; for !n, the error.
std::optional<std::size_t> Placeholder(std::string_view spelling, std::size_t i) {
  if ((spelling[i] != '$' && spelling[i] != '#' && spelling[i] != '!') ||
      i + 1 >= spelling.size() || spelling[i + 1] < '1' || spelling[i + 1] > '9') {
    return std::nullopt;
  }
  return static_cast<std::size_t>(spelling[i + 1] - '1');
}

bool Holds(const Condition& condition, const std::vector<FunctionArgument>& arguments) {
  if (condition.argument > arguments.size()) {
    return true;
  }
  const FunctionArgument& argument = arguments[condition.argument - 1];
  if (!condition.types.empty() && std::find(condition.types.begin(), condition.types.end(),
                                            argument.type) == condition.types.end()) {
    return false;
  }
  if (!condition.date_parts.empty() &&
      (!argument.date_part || std::find(condition.date_parts.begin(), condition.date_parts.end(),
                                        *argument.date_part) == condition.date_parts.end())) {
    return false;
  }
  if (condition.rounding_mode && argument.rounding_mode != *condition.rounding_mode) {
    return false;
  }
  return !condition.string_literal || argument.string_literal == *condition.string_literal;
}

bool Matches(const Rule& rule, const std::vector<FunctionArgument>& arguments) {
  if (arguments.size() < rule.arity.min || arguments.size() > rule.arity.max) {
    return false;
  }
  for (std::size_t i = 0; i < rule.spelling.size(); ++i) {
    if (const auto index = Placeholder(rule.spelling, i);
        index && rule.spelling[i] == '#' &&
        (*index >= arguments.size() || !arguments[*index].date_part)) {
      return false;
    }
  }
  return std::all_of(rule.conditions.begin(), rule.conditions.end(),
                     [&](const Condition& condition) { return Holds(condition, arguments); });
}

// SQL that is as cheap and as stable to repeat as a reference to it: a column, a number or a
// string without quotes in it, possibly negated or cast to a type such as BIGINT.
bool Trivial(std::string_view sql) {
  constexpr std::string_view kCast = "CAST(";
  if (sql.starts_with(kCast) && sql.ends_with(")")) {
    const std::string_view inner = sql.substr(kCast.size(), sql.size() - kCast.size() - 1);
    const std::size_t as = inner.rfind(" AS ");
    return as != std::string_view::npos && Trivial(inner.substr(0, as)) &&
           Trivial(inner.substr(as + 4));
  }
  if (sql.size() >= 2 && sql.front() == '-' && sql[1] != '-') {
    sql.remove_prefix(1);
  }
  if (sql.size() >= 2 && sql.front() == '\'' && sql.back() == '\'') {
    return sql.find('\'', 1) == sql.size() - 1;
  }
  return !sql.empty() && std::all_of(sql.begin(), sql.end(), [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
           c == '.' || c == '"';
  });
}

// The rule's spelling with each !n replaced by the SQL raising error n.
std::string WithErrors(const Rule& rule, bool safe) {
  std::string spelling;
  for (std::size_t i = 0; i < rule.spelling.size(); ++i) {
    const auto index = Placeholder(rule.spelling, i);
    if (index && rule.spelling[i] == '!') {
      spelling += Raise(rule.errors.at(*index), safe);
      ++i;
    } else {
      spelling += rule.spelling[i];
    }
  }
  return spelling;
}

std::string Expand(const Rule& rule, const std::vector<FunctionArgument>& arguments, bool safe) {
  const std::string spelling = WithErrors(rule, safe);
  std::vector<std::string> sql;
  sql.reserve(std::max<std::size_t>(arguments.size(), rule.arity.max));
  for (const FunctionArgument& argument : arguments) {
    sql.push_back(argument.sql);
  }
  // An optional argument without a default must not appear in the spelling.
  for (std::size_t i = arguments.size(); i - rule.arity.min < rule.defaults.size(); ++i) {
    sql.emplace_back(rule.defaults[i - rule.arity.min]);
  }
  std::vector<int> uses(sql.size());
  for (std::size_t i = 0; i < spelling.size(); ++i) {
    if (const auto index = Placeholder(spelling, i); index && spelling[i] == '$') {
      ++uses.at(*index);
    }
  }
  // Bind the arguments once when one would otherwise be evaluated twice, keeping the call an
  // expression that CASE and IF can short-circuit.
  bool bind = false;
  for (std::size_t i = 0; i < sql.size(); ++i) {
    bind = bind || (uses[i] > 1 && !Trivial(sql[i]));
  }
  std::vector<std::string> references = sql;
  std::string fields;
  if (bind) {
    // Trivial arguments stay as they are, so a literal that DuckDB needs as a constant, such as a
    // rounding precision, remains one.
    for (std::size_t i = 0; i < sql.size(); ++i) {
      if (uses[i] == 0 || Trivial(sql[i])) {
        continue;
      }
      const std::string field = "a" + std::to_string(i + 1);
      fields += (fields.empty() ? "" : ", ") + field + " := " + sql[i];
      references[i] = "_fn." + field;
    }
  }
  std::string body;
  for (std::size_t i = 0; i < spelling.size(); ++i) {
    if (const auto index = Placeholder(spelling, i)) {
      body += spelling[i] == '$' ? references.at(*index) : sql.at(*index);
      ++i;
    } else {
      body += spelling[i];
    }
  }
  if (!bind) {
    return body;
  }
  return "list_transform([struct_pack(" + fields + ")], _fn -> " + body + ")[1]";
}

std::string Invoke(std::string_view function, const std::vector<FunctionArgument>& arguments) {
  std::string sql = std::string(function) + "(";
  for (std::size_t i = 0; i < arguments.size(); ++i) {
    sql += (i == 0 ? "" : ", ") + arguments[i].sql;
  }
  return sql + ")";
}

// Substitutes the translated arguments for each $n in `spelling`.
std::string Substitute(std::string_view spelling, const std::vector<std::string>& arguments) {
  std::string sql;
  for (std::size_t i = 0; i < spelling.size(); ++i) {
    if (const auto index = Placeholder(spelling, i); index && spelling[i] == '$') {
      sql += arguments.at(*index);
      ++i;
    } else {
      sql += spelling[i];
    }
  }
  return sql;
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

const FunctionEntry* FindFunction(std::string_view upper_name) {
  const auto entry = Registry().find(upper_name);
  return entry == Registry().end() ? nullptr : &entry->second;
}

std::optional<std::string> TranslateFunction(std::string_view upper_name,
                                             const std::vector<FunctionArgument>& arguments,
                                             bool safe) {
  const FunctionEntry* entry = FindFunction(upper_name);
  if (entry == nullptr) {
    return std::nullopt;
  }
  switch (entry->implementation) {
    case Implementation::kSame:
      return Invoke(upper_name, arguments);
    case Implementation::kRenamed:
      return Invoke(entry->target, arguments);
    case Implementation::kRules:
    case Implementation::kBackend:
      // A function with rules is translated by them alone, so a call no rule matches, such as
      // one with an argument type the rules leave out, stays unsupported.
      for (const Rule& rule : entry->rules) {
        if (Matches(rule, arguments)) {
          return Expand(rule, arguments, safe);
        }
      }
      return std::nullopt;
    default:
      return std::nullopt;
  }
}

std::vector<std::string> AggregateArguments(const AggregateRule& rule,
                                            const std::vector<std::string>& arguments) {
  if (rule.arguments.empty()) {
    return arguments;
  }
  std::vector<std::string> given = arguments;
  for (std::size_t i = given.size(); i < rule.defaults.size(); ++i) {
    given.emplace_back(rule.defaults[i]);
  }
  std::vector<std::string> sql;
  sql.reserve(rule.arguments.size());
  for (const std::string_view spelling : rule.arguments) {
    sql.push_back(Substitute(spelling, given));
  }
  return sql;
}

}  // namespace bigquery_emulator_duckdb::translator
