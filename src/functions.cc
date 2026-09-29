#include "src/functions.h"

#include <algorithm>
#include <cstddef>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace bigquery_emulator_duckdb {
namespace {

using enum ArgumentType;

// The argument counts a rule accepts.
struct Arity {
  // NOLINTNEXTLINE(google-explicit-constructor): a bare count reads best in the table.
  Arity(std::size_t count) : min(count), max(count) {}
  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the order reads as a range.
  Arity(std::size_t min, std::size_t max) : min(min), max(max) {}

  std::size_t min;
  std::size_t max;
};

// What argument `argument` (counted from 1) has to be for a rule to apply. A condition on an
// optional argument the call leaves out holds.
struct Condition {
  std::size_t argument;
  // Any of these types; empty accepts any type.
  std::vector<ArgumentType> types;
  // Any of these date parts; empty accepts any argument.
  std::vector<std::string_view> date_parts;
  // A STRING literal with this value.
  std::optional<std::string_view> string_literal;
};

Condition Is(std::size_t argument, std::initializer_list<ArgumentType> types) {
  return {.argument = argument, .types = types};
}

Condition Part(std::size_t argument, std::initializer_list<std::string_view> date_parts) {
  return {.argument = argument, .date_parts = date_parts};
}

Condition SubDay(std::size_t argument) {
  return Part(argument, {"hour", "minute", "second", "millisecond", "microsecond"});
}

Condition Literal(std::size_t argument, std::string_view value) {
  return {.argument = argument, .string_literal = value};
}

// A DuckDB spelling of a BigQuery function. In the spelling, $n is argument n and #n argument
// n, which has to be a date part, as a lower case string literal. An argument that the spelling
// uses more than once is evaluated once, so volatile arguments stay consistent.
struct Rule {
  Arity arity;
  std::string spelling;
  std::vector<Condition> conditions = {};
  // The spellings of the optional arguments, from the first one past arity.min on.
  std::vector<std::string_view> defaults = {};
};

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
  return {{1, "CAST(CAST($1 AS TIMESTAMP) AS " + target + ")", {Is(1, {kTimestamp})}},
          {1, "CAST($1 AS " + target + ")"},
          {2, "CAST(timezone($2, $1) AS " + target + ")", {Is(1, {kTimestamp})}}};
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

// STRPOS and INSTR without a position or occurrence. A match in the spaced digits starts on a
// byte, three characters each.
std::vector<Rule> Position() {
  return {
      {2, "strpos($1, $2)", {Is(1, {kString})}},
      {2, "((strpos(" + Spaced("$1") + ", " + Spaced("$2") + ") + 2) // 3)", {Is(1, {kBytes})}}};
}

// LPAD and RPAD. DuckDB has no default pad; the BYTES one is b' '.
std::vector<Rule> Pad(const std::string& function) {
  return {
      {{2, 3}, function + "($1, CAST($2 AS INTEGER), $3)", {Is(1, {kString})}, {"' '"}},
      {2, "unhex(" + function + "(hex($1), CAST(2 * $2 AS INTEGER), '20'))", {Is(1, {kBytes})}},
      {3, "unhex(" + function + "(hex($1), CAST(2 * $2 AS INTEGER), hex($3)))", {Is(1, {kBytes})}}};
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
  auto rules = Same(function, {1, 2}, {Is(1, {kString})});
  rules.push_back(
      {2,
       "unhex(replace(regexp_replace(" + Spaced("$1") + ", '" + regex + "', '', 'g'), ' ', ''))",
       {Is(1, {kBytes})}});
  return rules;
}

// SUBSTR and SUBSTRING. BigQuery starts at the first character for a position of 0 or one
// before the start, where DuckDB takes as many characters fewer.
std::vector<Rule> Substr() {
  const auto start = [](const std::string& length) {
    return "CASE WHEN $2 > 0 THEN $2 WHEN $2 = 0 OR $2 < -" + length + " THEN 1 ELSE " + length +
           " + $2 + 1 END";
  };
  const std::string negative =
      "CASE WHEN $3 < 0 THEN error('Third argument in SUBSTR() cannot be negative') ELSE ";
  return {
      {2, "substr($1, " + start("length($1)") + ")", {Is(1, {kString})}},
      {3, negative + "substr($1, " + start("length($1)") + ", $3) END", {Is(1, {kString})}},
      {2, "unhex(substr(hex($1), 2 * (" + start("octet_length($1)") + ") - 1))", {Is(1, {kBytes})}},
      {3,
       negative + "unhex(substr(hex($1), 2 * (" + start("octet_length($1)") + ") - 1, 2 * $3)) END",
       {Is(1, {kBytes})}}};
}

// The reciprocal of a DuckDB function, which BigQuery reports as an error at zero where DuckDB
// returns infinity.
std::vector<Rule> Reciprocal(std::string_view function) {
  const std::string value = std::string(function) + "($1)";
  return {{1, "CASE WHEN " + value +
                  " = 0 THEN error('Floating point error: division by zero') "
                  "ELSE 1 / " +
                  value + " END"}};
}

std::vector<Rule> Concat(std::initializer_list<std::vector<Rule>> groups) {
  std::vector<Rule> rules;
  for (const auto& group : groups) {
    rules.insert(rules.end(), group.begin(), group.end());
  }
  return rules;
}

const std::unordered_map<std::string_view, std::vector<Rule>>& Rules() {
  static const auto* const kRules = new std::unordered_map<std::string_view, std::vector<Rule>>{
      // A division by zero is an error in BigQuery and +Inf in DuckDB, so SAFE_DIVIDE has to
      // make the zero itself disappear.
      {"SAFE_DIVIDE", {{2, "($1 / NULLIF($2, 0))"}}},
      {"IEEE_DIVIDE", {{2, "(CAST($1 AS DOUBLE) / CAST($2 AS DOUBLE))"}}},
      // DuckDB's log() is the base 10 logarithm, BigQuery's LOG() the natural one, and the two
      // argument form takes the base first rather than last.
      {"LOG", {{1, "ln($1)"}, {2, "log($2, $1)"}}},
      // DuckDB takes the digits as an INTEGER.
      {"ROUND", {{1, "round($1)"}, {2, "round($1, CAST($2 AS INTEGER))"}}},
      {"TRUNC", {{1, "trunc($1)"}, {2, "trunc($1, CAST($2 AS INTEGER))"}}},
      {"SEC", Reciprocal("cos")},
      {"CSC", Reciprocal("sin")},
      {"SECH", Reciprocal("cosh")},
      {"CSCH", Reciprocal("sinh")},
      {"COTH", Reciprocal("tanh")},
      {"BIT_COUNT", {{1, "bit_count($1)", {Is(1, {kInt64})}}}},

      // Date and time arithmetic: DuckDB uses the operators and puts the date part first, as a
      // string rather than as a keyword.
      {"DATE_ADD", {{2, "CAST($1 + $2 AS DATE)"}}},
      {"DATE_SUB", {{2, "CAST($1 - $2 AS DATE)"}}},
      {"DATETIME_ADD", {{2, "($1 + $2)"}}},
      {"DATETIME_SUB", {{2, "($1 - $2)"}}},
      {"TIMESTAMP_ADD", {{2, "($1 + $2)"}}},
      {"TIMESTAMP_SUB", {{2, "($1 - $2)"}}},
      {"TIME_ADD", {{2, "($1 + $2)"}}},
      {"TIME_SUB", {{2, "($1 - $2)"}}},
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
                            {2, "($1 + $2)", {Is(1, {kDate}), Is(2, {kTime})}}},
                           Civil("TIMESTAMP")})},
      {"TIMESTAMP",
       {{1, "CAST($1 AS TIMESTAMPTZ)"},
        // A civil time in the given zone; a string with its own offset keeps the offset.
        {2, "timezone($2, CAST($1 AS TIMESTAMP))", {Is(1, {kDate, kDatetime})}}}},

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
      {"LENGTH", {{1, "octet_length($1)", {Is(1, {kBytes})}}, {1, "length($1)"}}},
      {"BYTE_LENGTH",
       {{1, "strlen($1)", {Is(1, {kString})}}, {1, "octet_length($1)", {Is(1, {kBytes})}}}},
      {"INSTR", Position()},
      {"STRPOS", Position()},
      {"LEFT",
       {{2,
         "CASE WHEN $2 < 0 THEN error('LEFT length must be non-negative') ELSE left($1, $2) END",
         {Is(1, {kString})}},
        {2,
         "CASE WHEN $2 < 0 THEN error('LEFT length must be non-negative') ELSE unhex(left(hex($1), "
         "2 * $2)) END",
         {Is(1, {kBytes})}}}},
      {"RIGHT",
       {{2,
         "CASE WHEN $2 < 0 THEN error('RIGHT length must be non-negative') ELSE right($1, $2) END",
         {Is(1, {kString})}},
        {2,
         "CASE WHEN $2 < 0 THEN error('RIGHT length must be non-negative') ELSE "
         "unhex(right(hex($1), 2 * $2)) END",
         {Is(1, {kBytes})}}}},
      {"LPAD", Pad("lpad")},
      {"RPAD", Pad("rpad")},
      {"SPLIT",
       {{{1, 2}, "split($1, $2)", {Is(1, {kString})}, {"','"}},
        // An empty delimiter splits the value into its bytes.
        {2,
         "list_transform(CASE WHEN octet_length($2) = 0 THEN regexp_extract_all(hex($1), '..') "
         "ELSE list_transform(string_split(" +
             Spaced("$1") + ", " + Spaced("$2") +
             "), _p -> replace(_p, ' ', '')) END, _p -> unhex(_p))",
         {Is(1, {kBytes})}}}},
      {"TRANSLATE", {{3, "translate($1, $2, $3)", {Is(1, {kString})}}}},
      {"ASCII",
       {{1, "ascii($1)", {Is(1, {kString})}},
        {1,
         "CASE WHEN octet_length($1) = 0 THEN 0 ELSE CAST('0x' || left(hex($1), 2) AS BIGINT) END",
         {Is(1, {kBytes})}}}},
      {"UNICODE", {{1, "CASE WHEN $1 = '' THEN 0 ELSE unicode($1) END", {Is(1, {kString})}}}},
      {"CHR", {{1, "CASE WHEN $1 = 0 THEN '' ELSE chr(CAST($1 AS INTEGER)) END"}}},
      {"NORMALIZE", {{1, "nfc_normalize($1)", {Is(1, {kString})}}}},
      // REGEXP_REPLACE replaces every occurrence; DuckDB needs the global flag for that.
      {"REGEXP_REPLACE", {{3, "regexp_replace($1, $2, $3, 'g')", {Is(1, {kString})}}}},
      {"REGEXP_CONTAINS", {{2, "regexp_matches($1, $2)", {Is(1, {kString})}}}},
      // A BLOB cast to VARCHAR escapes every byte but printable ASCII as \xHH, so the case
      // mapping only touches ASCII letters, and the escapes read back in either case but \X.
      {"LOWER",
       {{1, "lower($1)", {Is(1, {kString})}},
        {1, "CAST(lower(CAST($1 AS VARCHAR)) AS BLOB)", {Is(1, {kBytes})}}}},
      {"UPPER",
       {{1, "upper($1)", {Is(1, {kString})}},
        {1, "CAST(replace(upper(CAST($1 AS VARCHAR)), '\\X', '\\x') AS BLOB)", {Is(1, {kBytes})}}}},
      // Reversing the digits reverses the bytes and swaps the two digits of each.
      {"REVERSE",
       {{1, "reverse($1)", {Is(1, {kString})}},
        {1,
         "unhex(regexp_replace(reverse(hex($1)), '(.)(.)', '\\2\\1', 'g'))",
         {Is(1, {kBytes})}}}},
      {"TRIM", Trim("trim", "^(?:@)+|(?:@)+$")},
      {"LTRIM", Trim("ltrim", "^(?:@)+")},
      {"RTRIM", Trim("rtrim", "(?:@)+$")},
      {"SUBSTR", Substr()},
      {"SUBSTRING", Substr()},
      {"STARTS_WITH",
       {{2, "starts_with($1, $2)", {Is(1, {kString})}},
        {2, "starts_with(hex($1), hex($2))", {Is(1, {kBytes})}}}},
      {"ENDS_WITH",
       {{2, "ends_with($1, $2)", {Is(1, {kString})}},
        {2, "ends_with(hex($1), hex($2))", {Is(1, {kBytes})}}}},
      {"REPLACE",
       {{3, "replace($1, $2, $3)", {Is(1, {kString})}},
        {3,
         "unhex(replace(replace(" + Spaced("$1") + ", " + Spaced("$2") + ", " + Spaced("$3") +
             "), ' ', ''))",
         {Is(1, {kBytes})}}}},

      // Hashes are BYTES in BigQuery and hexadecimal strings in DuckDB.
      {"MD5", {{1, "unhex(md5($1))"}}},
      {"SHA1", {{1, "unhex(sha1($1))"}}},
      {"SHA256", {{1, "unhex(sha256($1))"}}},
      // DuckDB lacks these; src/backend.cc registers GoogleSQL's implementations as bq_*.
      // encode() takes a STRING's UTF-8 bytes.
      {"SHA512",
       {{1, "bq_sha512(encode($1))", {Is(1, {kString})}}, {1, "bq_sha512($1)", {Is(1, {kBytes})}}}},
      {"FARM_FINGERPRINT",
       {{1, "bq_farm_fingerprint(encode($1))", {Is(1, {kString})}},
        {1, "bq_farm_fingerprint($1)", {Is(1, {kBytes})}}}},
      {"INITCAP", {{1, "bq_initcap($1)"}, {2, "bq_initcap_delimiters($1, $2)"}}},
      // Without max_distance, the distance is not capped.
      {"EDIT_DISTANCE",
       {{{2, 3}, "bq_edit_distance($1, $2, $3)", {Is(1, {kString})}, {"9223372036854775807"}},
        {{2, 3},
         "bq_edit_distance_bytes($1, $2, $3)",
         {Is(1, {kBytes})},
         {"9223372036854775807"}}}},
      {"REGEXP_INSTR",
       {{{2, 5}, "bq_regexp_instr($1, $2, $3, $4, $5)", {Is(1, {kString})}, {"1", "1", "0"}},
        {{2, 5}, "bq_regexp_instr_bytes($1, $2, $3, $4, $5)", {Is(1, {kBytes})}, {"1", "1", "0"}}}},
      {"TO_HEX", {{1, "lower(hex($1))"}}},
      {"FROM_HEX", {{1, "unhex($1)", {Is(1, {kString})}}}},
      {"TO_BASE64", {{1, "to_base64($1)"}}},
      {"FROM_BASE64", {{1, "from_base64($1)", {Is(1, {kString})}}}},

      // JSON. Only the exact wide number mode keeps DuckDB's numbers as they are.
      {"PARSE_JSON", {{{1, 2}, "json($1)", {Literal(2, "exact")}}}},
      {"TO_JSON_STRING", {{1, "CAST(to_json($1) AS VARCHAR)"}}},
      {"JSON_TYPE",
       {{1,
         "CASE json_type($1) WHEN 'OBJECT' THEN 'object' WHEN 'ARRAY' THEN 'array' "
         "WHEN 'VARCHAR' THEN 'string' WHEN 'BOOLEAN' THEN 'boolean' WHEN 'NULL' THEN 'null' "
         "WHEN 'BIGINT' THEN 'number' WHEN 'UBIGINT' THEN 'number' "
         "WHEN 'DOUBLE' THEN 'number' END"}}},

      {"ERROR", {{1, "error($1)"}}},
      {"ARRAY_REVERSE", {{1, "list_reverse($1)"}}},
      // DuckDB's array_to_string() skips NULL elements and has no NULL text.
      {"ARRAY_TO_STRING",
       {{2, "array_to_string($1, $2)", {Is(2, {kString})}},
        {3, "array_to_string(list_transform($1, _e -> coalesce(_e, $3)), $2)", {Is(2, {kString})}},
        {2,
         "unhex(array_to_string(list_transform($1, _e -> hex(_e)), hex($2)))",
         {Is(2, {kBytes})}},
        {3,
         "unhex(array_to_string(list_transform($1, _e -> hex(coalesce(_e, $3))), hex($2)))",
         {Is(2, {kBytes})}}}},
      // generate_series() has no floating point overload, and returns an empty list for a zero
      // step, which BigQuery rejects.
      {"GENERATE_ARRAY",
       {{{2, 3},
         "CASE WHEN $3 = 0 THEN error('Sequence step cannot be 0.') ELSE generate_series($1, $2, "
         "$3) END",
         {Is(1, {kInt64}), Is(2, {kInt64}), Is(3, {kInt64})},
         {"1"}}}},
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
      {"JSON_EXTRACT_SCALAR", "json_extract_string"},
      {"JSON_QUERY", "json_extract"},
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

// The argument a $n or #n at spelling[i] refers to, counted from 0.
std::optional<std::size_t> Placeholder(std::string_view spelling, std::size_t i) {
  if ((spelling[i] != '$' && spelling[i] != '#') || i + 1 >= spelling.size() ||
      spelling[i + 1] < '1' || spelling[i + 1] > '9') {
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
// string without quotes in it.
bool Trivial(std::string_view sql) {
  if (sql.size() >= 2 && sql.front() == '\'' && sql.back() == '\'') {
    return sql.find('\'', 1) == sql.size() - 1;
  }
  return std::all_of(sql.begin(), sql.end(), [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
           c == '.' || c == '"';
  });
}

std::string Expand(const Rule& rule, const std::vector<FunctionArgument>& arguments) {
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
  for (std::size_t i = 0; i < rule.spelling.size(); ++i) {
    if (const auto index = Placeholder(rule.spelling, i); index && rule.spelling[i] == '$') {
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
    for (std::size_t i = 0; i < sql.size(); ++i) {
      if (uses[i] == 0) {
        continue;
      }
      const std::string field = "a" + std::to_string(i + 1);
      fields += (fields.empty() ? "" : ", ") + field + " := " + sql[i];
      references[i] = "_fn." + field;
    }
  }
  std::string body;
  for (std::size_t i = 0; i < rule.spelling.size(); ++i) {
    if (const auto index = Placeholder(rule.spelling, i)) {
      body += rule.spelling[i] == '$' ? references.at(*index) : sql.at(*index);
      ++i;
    } else {
      body += rule.spelling[i];
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

}  // namespace

std::optional<std::string> TranslateFunction(std::string_view upper_name,
                                             const std::vector<FunctionArgument>& arguments) {
  // A function with rules is translated by them alone, so a call no rule matches, such as one
  // with an argument type the rules leave out, stays unsupported.
  if (const auto rules = Rules().find(upper_name); rules != Rules().end()) {
    for (const Rule& rule : rules->second) {
      if (Matches(rule, arguments)) {
        return Expand(rule, arguments);
      }
    }
    return std::nullopt;
  }
  if (const auto renamed = FunctionNames().find(upper_name); renamed != FunctionNames().end()) {
    return Invoke(renamed->second, arguments);
  }
  if (PlainFunctions().contains(upper_name)) {
    return Invoke(upper_name, arguments);
  }
  return std::nullopt;
}

}  // namespace bigquery_emulator_duckdb
