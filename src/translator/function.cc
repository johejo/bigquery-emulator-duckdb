#include "googlesql/public/function.h"

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "googlesql/public/type.h"
#include "googlesql/public/value.h"
#include "googlesql/resolved_ast/resolved_ast.h"
#include "src/duckdb_sql.h"
#include "src/functions.h"
#include "src/translator/internal.h"

namespace bigquery_emulator_duckdb::translator {
namespace {

// Replaces each error(...) call in `sql` with NULL, skipping over string literals; nullopt when
// a call is not closed.
std::optional<std::string> WithoutErrors(const std::string& sql) {
  static constexpr std::string_view kError = "error(";
  std::string result;
  size_t i = 0;
  while (i < sql.size()) {
    if (sql[i] == '\'') {
      const size_t end = sql.find('\'', i + 1);
      if (end == std::string::npos) {
        return std::nullopt;
      }
      result.append(sql, i, end + 1 - i);
      i = end + 1;
      continue;
    }
    const bool call = sql.compare(i, kError.size(), kError) == 0 &&
                      (i == 0 || (std::isalnum(static_cast<unsigned char>(sql[i - 1])) == 0 &&
                                  sql[i - 1] != '_'));
    if (!call) {
      result += sql[i++];
      continue;
    }
    int depth = 0;
    size_t j = i + kError.size() - 1;
    for (; j < sql.size(); ++j) {
      if (sql[j] == '\'') {
        j = sql.find('\'', j + 1);
        if (j == std::string::npos) {
          return std::nullopt;
        }
      } else if (sql[j] == '(') {
        ++depth;
      } else if (sql[j] == ')' && --depth == 0) {
        break;
      }
    }
    if (j >= sql.size()) {
      return std::nullopt;
    }
    result += "NULL";
    i = j + 1;
  }
  return result;
}

// Date parts are enum literals after analysis, not SQL identifier expressions.
std::optional<std::string> DatePart(const googlesql::ResolvedExpr& expr) {
  if (!expr.Is<googlesql::ResolvedLiteral>()) {
    return std::nullopt;
  }
  const auto& value = expr.GetAs<googlesql::ResolvedLiteral>()->value();
  if (value.is_null() || !value.type()->IsEnum()) {
    return std::nullopt;
  }
  const std::string part = ToLowerAscii(value.EnumDisplayName());
  static const std::set<std::string> supported = {
      "year",   "quarter",     "month",       "week",    "day",     "hour",      "minute",
      "second", "millisecond", "microsecond", "isoyear", "isoweek", "dayofweek", "dayofyear"};
  return supported.contains(part) ? std::optional<std::string>(part) : std::nullopt;
}

// The name of a rounding mode, such as ROUND_HALF_EVEN, which is also an enum literal after
// analysis.
std::optional<std::string> RoundingMode(const googlesql::ResolvedExpr& expr) {
  if (!expr.Is<googlesql::ResolvedLiteral>()) {
    return std::nullopt;
  }
  const auto& value = expr.GetAs<googlesql::ResolvedLiteral>()->value();
  if (value.is_null() || !value.type()->Equals(googlesql::types::RoundingModeEnumType())) {
    return std::nullopt;
  }
  return value.EnumDisplayName();
}

// A bucket width INTERVAL of a single part, as a count of months, days or microseconds.
// INTERVAL n PART resolves to $interval(n, PART), and an INTERVAL string to a literal.
struct BucketWidth {
  std::string unit;
  // The n of INTERVAL n PART, or null for a literal, whose count is all in `factor`.
  const googlesql::ResolvedExpr* count = nullptr;
  int64_t factor = 1;
};

std::optional<BucketWidth> BucketWidthOf(const googlesql::ResolvedExpr& expr) {
  if (expr.Is<googlesql::ResolvedFunctionCall>()) {
    const auto* call = expr.GetAs<googlesql::ResolvedFunctionCall>();
    if (call->function()->Name() != "$interval" || call->argument_list_size() != 2) {
      return std::nullopt;
    }
    static const std::map<std::string, std::pair<std::string, int64_t>> units = {
        {"year", {"months", 12}},
        {"quarter", {"months", 3}},
        {"month", {"months", 1}},
        {"week", {"days", 7}},
        {"day", {"days", 1}},
        {"hour", {"micros", 3600000000}},
        {"minute", {"micros", 60000000}},
        {"second", {"micros", 1000000}},
        {"millisecond", {"micros", 1000}},
        {"microsecond", {"micros", 1}}};
    const auto part = DatePart(*call->argument_list(1));
    const auto unit = part ? units.find(*part) : units.end();
    if (unit == units.end()) {
      return std::nullopt;
    }
    return BucketWidth{
        .unit = unit->second.first, .count = call->argument_list(0), .factor = unit->second.second};
  }
  if (!expr.Is<googlesql::ResolvedLiteral>()) {
    return std::nullopt;
  }
  const auto& value = expr.GetAs<googlesql::ResolvedLiteral>()->value();
  if (value.is_null() || !value.type()->IsInterval() ||
      value.interval_value().get_nano_fractions() != 0) {
    return std::nullopt;
  }
  // A zero width counts as days, which raises the error BigQuery raises for every input type.
  std::vector<BucketWidth> parts;
  for (const auto& [unit, count] :
       {std::pair<std::string, int64_t>{"months", value.interval_value().get_months()},
        {"days", value.interval_value().get_days()},
        {"micros", value.interval_value().get_micros()}}) {
    if (count != 0) {
      parts.push_back({.unit = unit, .factor = count});
    }
  }
  if (parts.size() > 1) {
    return std::nullopt;
  }
  return parts.empty() ? BucketWidth{.unit = "days", .factor = 0} : parts[0];
}

ArgumentType TypeOf(const googlesql::Type& type) {
  if (type.IsString()) {
    return ArgumentType::kString;
  }
  if (type.IsBytes()) {
    return ArgumentType::kBytes;
  }
  if (type.IsInt64()) {
    return ArgumentType::kInt64;
  }
  if (type.IsDate()) {
    return ArgumentType::kDate;
  }
  if (type.IsDatetime()) {
    return ArgumentType::kDatetime;
  }
  if (type.IsTime()) {
    return ArgumentType::kTime;
  }
  if (type.IsTimestamp()) {
    return ArgumentType::kTimestamp;
  }
  if (type.IsJson()) {
    return ArgumentType::kJson;
  }
  return ArgumentType::kOther;
}

std::optional<std::string> StringLiteral(const googlesql::ResolvedExpr& expr) {
  if (!expr.Is<googlesql::ResolvedLiteral>()) {
    return std::nullopt;
  }
  const auto& value = expr.GetAs<googlesql::ResolvedLiteral>()->value();
  if (value.is_null() || !value.type()->IsString()) {
    return std::nullopt;
  }
  return value.string_value();
}

// A literal JSONPath as a DuckDB path literal. The legacy JSON_EXTRACT functions escape keys as
// ['a.b'], the standard ones as ."a.b". Paths outside this subset are unsupported.
std::optional<std::string> JsonPath(const googlesql::ResolvedExpr& expr, bool legacy) {
  const auto path = StringLiteral(expr);
  if (!path || !path->starts_with("$")) {
    return std::nullopt;
  }
  std::string sql = "$";
  for (size_t i = 1; i < path->size();) {
    const char c = (*path)[i++];
    std::optional<std::string> element;
    if (c == '.' && !legacy && i < path->size() && (*path)[i] == '"') {
      const size_t end = path->find('"', i + 1);
      if (end == std::string::npos ||
          path->substr(i + 1, end - i - 1).find('\\') != std::string::npos) {
        return std::nullopt;
      }
      element = JsonPathKey(path->substr(i + 1, end - i - 1));
      i = end + 1;
    } else if (c == '.') {
      const size_t end = std::min(path->find_first_of(".[", i), path->size());
      const std::string key = path->substr(i, end - i);
      if (key.find_first_of("\"'] \t\n\r") != std::string::npos) {
        return std::nullopt;
      }
      element = JsonPathKey(key);
      i = end;
    } else if (c == '[' && legacy && i < path->size() && (*path)[i] == '\'') {
      const size_t end = path->find('\'', i + 1);
      if (end == std::string::npos || end + 1 >= path->size() || (*path)[end + 1] != ']') {
        return std::nullopt;
      }
      element = JsonPathKey(path->substr(i + 1, end - i - 1));
      i = end + 2;
    } else if (c == '[') {
      const size_t end = path->find(']', i);
      const std::string index = end == std::string::npos ? "" : path->substr(i, end - i);
      if (index.empty() || index.size() > 18 ||
          index.find_first_not_of("0123456789") != std::string::npos) {
        return std::nullopt;
      }
      element = "[" + index + "]";
      i = end + 1;
    }
    if (!element) {
      return std::nullopt;
    }
    sql += *element;
  }
  return QuoteLiteral(sql);
}

// The number of capturing groups in an RE2 pattern, or nullopt if it cannot tell.
std::optional<size_t> CapturingGroups(std::string_view pattern) {
  size_t groups = 0;
  for (size_t i = 0; i < pattern.size(); ++i) {
    if (pattern[i] == '\\') {
      if (i + 1 < pattern.size() && pattern[i + 1] == 'Q') {
        return std::nullopt;
      }
      ++i;
    } else if (pattern[i] == '[') {
      // A ] right after [ or [^ is literal.
      size_t j = i + 1;
      if (j < pattern.size() && pattern[j] == '^') {
        ++j;
      }
      if (j < pattern.size() && pattern[j] == ']') {
        ++j;
      }
      for (; j < pattern.size() && pattern[j] != ']'; ++j) {
        if (pattern[j] == '\\') {
          ++j;
        }
      }
      i = j;
    } else if (pattern[i] == '(') {
      const std::string_view rest = pattern.substr(i + 1);
      if (!rest.starts_with("?") || rest.starts_with("?P<") ||
          (rest.starts_with("?<") && !rest.starts_with("?<=") && !rest.starts_with("?<!"))) {
        ++groups;
      }
    }
  }
  return groups;
}

// The DuckDB spelling of a scalar call over already translated arguments. Only the functions
// that the rules of src/functions.cc cannot express are spelled here; see TranslateFunction.
std::optional<std::string> Call(const googlesql::ResolvedFunctionCall& call,
                                const std::string& name, const std::vector<std::string>& args) {
  const size_t n = args.size();
  std::vector<FunctionArgument> arguments;
  for (size_t i = 0; i < n; ++i) {
    const googlesql::ResolvedExpr& argument = *call.argument_list(static_cast<int>(i));
    arguments.push_back({.sql = args[i],
                         .type = TypeOf(*argument.type()),
                         .date_part = DatePart(argument),
                         .rounding_mode = RoundingMode(argument),
                         .string_literal = StringLiteral(argument)});
  }
  if (name == "$MAKE_ARRAY") {
    return "[" + Join(args, ", ") + "]";
  }
  if ((name == "$AND" || name == "$OR") && n >= 2) {
    return "(" + Join(args, name == "$AND" ? " AND " : " OR ") + ")";
  }
  if (name == "$IN" && n >= 2) {
    const std::vector<std::string> values(args.begin() + 1, args.end());
    return "(" + args[0] + " IN (" + Join(values, ", ") + "))";
  }
  if (name == "$CASE_NO_VALUE" || name == "$CASE_WITH_VALUE") {
    const size_t start = name == "$CASE_WITH_VALUE" ? 1 : 0;
    if (n < start + 3 || (n - start) % 2 != 1) {
      return std::nullopt;
    }
    std::string sql = "CASE ";
    if (start == 1) {
      sql += args[0] + " ";
    }
    for (size_t i = start; i + 1 < n; i += 2) {
      sql += "WHEN " + args[i] + " THEN " + args[i + 1] + " ";
    }
    return "(" + sql + "ELSE " + args.back() + " END)";
  }
  const auto type = [&](size_t i) { return call.argument_list(static_cast<int>(i))->type(); };
  // The width is already a count of months, days or microseconds, see BucketWidthOf. Months
  // count from the origin in the calendar, while days and microseconds are fixed lengths;
  // TIMESTAMP and DATETIME take a day as 24 hours.
  if ((name == "DATE_BUCKET" || name == "DATETIME_BUCKET" || name == "TIMESTAMP_BUCKET") &&
      (n == 2 || n == 3)) {
    const auto width = BucketWidthOf(*call.argument_list(1));
    const googlesql::Type* input = type(0);
    if (!width || (input->IsDate() && width->unit == "micros") ||
        (!input->IsDate() && width->unit == "months")) {
      return std::nullopt;
    }
    std::string bucket;
    if (input->IsDate() && width->unit == "days") {
      bucket = "_bk.x - CAST((((_bk.x - _bk.o) % _bk.w) + _bk.w) % _bk.w AS INTEGER)";
    } else if (input->IsDate()) {
      // A day past the origin's day of the month starts the next bucket, and last days of the
      // month count as the same day.
      bucket =
          "list_transform([(year(_bk.x) - year(_bk.o)) * 12 + month(_bk.x) - month(_bk.o)], _m -> "
          "CAST(_bk.o + to_months(CAST(_m - _m % _bk.w - CASE WHEN _m % _bk.w < 0 OR (_m % _bk.w = "
          "0 AND NOT (_bk.o = last_day(_bk.o) AND _bk.x = last_day(_bk.x)) AND day(_bk.x) < "
          "day(_bk.o)) THEN _bk.w ELSE 0 END AS INTEGER)) AS DATE))[1]";
    } else {
      const std::string w = width->unit == "days" ? "(_bk.w * 86400000000)" : "_bk.w";
      bucket = "_bk.x - to_microseconds((((epoch_us(_bk.x) - epoch_us(_bk.o)) % " + w + ") + " + w +
               ") % " + w + ")";
    }
    const std::string origin = n == 3                ? args[2]
                               : input->IsDate()     ? "DATE '1950-01-01'"
                               : input->IsDatetime() ? "TIMESTAMP '1950-01-01 00:00:00'"
                                                     : "TIMESTAMPTZ '1950-01-01 00:00:00+00'";
    const std::string zero = input->IsTimestamp()
                                 ? "Zero bucket width INTERVAL is not allowed"
                                 : "Exactly one non-zero INTERVAL part in bucket width is required";
    return "list_transform([struct_pack(x := " + args[0] + ", w := " + args[1] +
           ", o := " + origin +
           ")], _bk -> CASE WHEN _bk.w < 0 THEN error('Negative bucket width INTERVAL is not "
           "allowed') WHEN _bk.w = 0 THEN error('" +
           zero + "') ELSE " + bucket + " END)[1]";
  }
  if ((name == "REGEXP_EXTRACT" || name == "REGEXP_EXTRACT_ALL") && n == 2 && type(0)->IsString()) {
    const auto pattern = StringLiteral(*call.argument_list(1));
    const auto groups = pattern ? CapturingGroups(*pattern) : std::nullopt;
    if (!groups) {
      return std::nullopt;
    }
    if (*groups > 1) {
      return "error('Regular expressions passed into extraction functions must not have more "
             "than 1 capturing group')";
    }
    const std::string group = std::to_string(*groups);
    if (name == "REGEXP_EXTRACT_ALL") {
      return "regexp_extract_all(" + args[0] + ", " + args[1] + ", " + group + ")";
    }
    // DuckDB returns an empty string rather than NULL when nothing matches.
    return "list_transform([" + args[0] + "], _rx -> CASE WHEN regexp_matches(_rx, " + args[1] +
           ") THEN regexp_extract(_rx, " + args[1] + ", " + group + ") END)[1]";
  }
  static const std::map<std::string, bool> json_extractors = {
      {"JSON_QUERY", false},       {"JSON_EXTRACT", true},
      {"JSON_VALUE", false},       {"JSON_EXTRACT_SCALAR", true},
      {"JSON_QUERY_ARRAY", false}, {"JSON_EXTRACT_ARRAY", true},
      {"JSON_VALUE_ARRAY", false}, {"JSON_EXTRACT_STRING_ARRAY", true}};
  if (const auto legacy = json_extractors.find(name); legacy != json_extractors.end() &&
                                                      (n == 1 || n == 2) &&
                                                      (type(0)->IsString() || type(0)->IsJson())) {
    const auto path = n == 1 ? std::optional<std::string>("'$'")
                             : JsonPath(*call.argument_list(1), legacy->second);
    if (!path) {
      return std::nullopt;
    }
    const bool strings = type(0)->IsString();
    const std::string value = "json_extract(_j, " + *path + ")";
    const std::string elements = "CAST(" + value + " AS JSON[])";
    std::string sql;
    if (name == "JSON_QUERY" || name == "JSON_EXTRACT") {
      // A JSON null in a STRING is SQL NULL, and in JSON the JSON null.
      if (!strings) {
        return "json_extract(" + args[0] + ", " + *path + ")";
      }
      sql = "CASE WHEN json_type(" + value + ") <> 'NULL' THEN " + value + " END";
    } else if (name == "JSON_VALUE" || name == "JSON_EXTRACT_SCALAR") {
      sql = "CASE WHEN json_type(" + value +
            ") NOT IN ('OBJECT', 'ARRAY') THEN json_extract_string(" + value + ", '$') END";
    } else if (name == "JSON_QUERY_ARRAY" || name == "JSON_EXTRACT_ARRAY") {
      // Indexing keeps JSON nulls, which a cast to JSON[] turns into SQL NULLs.
      sql = "CASE WHEN json_type(" + value + ") = 'ARRAY' THEN list_transform(range(CAST(" +
            "json_array_length(" + value + ") AS BIGINT)), _i -> json_extract(" + value +
            ", _i)) END";
    } else {
      // NULL unless every element is a scalar; JSON nulls become SQL NULLs.
      sql = "CASE WHEN json_type(" + value + ") = 'ARRAY' AND len(list_filter(" + elements +
            ", _e -> json_type(_e) IN ('OBJECT', 'ARRAY'))) = 0 THEN list_transform(" + elements +
            ", _e -> json_extract_string(_e, '$')) END";
    }
    // A malformed JSON string gives NULL rather than an error.
    if (strings) {
      sql = "CASE WHEN json_valid(_j) THEN " + sql + " END";
    }
    return "list_transform([" + args[0] + "], _j -> " + sql + ")[1]";
  }
  if (name == "$SUBSCRIPT" && n == 2 && type(0)->IsJson()) {
    if (type(1)->IsInt64()) {
      // DuckDB counts negative indexes from the end.
      return "list_transform([struct_pack(j := " + args[0] + ", i := " + args[1] +
             ")], _js -> CASE WHEN _js.i >= 0 THEN json_extract(_js.j, _js.i) END)[1]";
    }
    const auto key = StringLiteral(*call.argument_list(1));
    const auto path = key ? JsonPathKey(*key) : std::nullopt;
    if (!path) {
      return std::nullopt;
    }
    return "json_extract(" + args[0] + ", " + QuoteLiteral("$" + *path) + ")";
  }
  // The JSON functions in src/backend_functions.cc take and return JSON as its text.
  const auto json_text = [&](size_t i) {
    return type(i)->IsJson() ? "CAST(" + args[i] + " AS VARCHAR)"
                             : "CAST(to_json(" + args[i] + ") AS VARCHAR)";
  };
  // BigQuery's TO_JSON makes JSON null of NULL. Stringifying wide numbers is unsupported.
  if (name == "TO_JSON" &&
      (n == 1 || (n == 2 && call.argument_list(1)->Is<googlesql::ResolvedLiteral>() &&
                  call.argument_list(1)->GetAs<googlesql::ResolvedLiteral>()->value() ==
                      googlesql::Value::Bool(false)))) {
    return "coalesce(to_json(" + args[0] + "), JSON 'null')";
  }
  // JSON_REMOVE and JSON_SET take the paths one by one, in order.
  if (name == "JSON_REMOVE" && n >= 2) {
    std::string result = json_text(0);
    for (size_t i = 1; i < n; ++i) {
      result.insert(0, "bq_json_remove(").append(", ").append(args[i]).append(")");
    }
    return "json(" + result + ")";
  }
  if (name == "JSON_SET" && n >= 4 && n % 2 == 0) {
    std::string result = json_text(0);
    for (size_t i = 1; i + 1 < n; i += 2) {
      result.insert(0, "bq_json_set(")
          .append(", ")
          .append(args[i])
          .append(", ")
          .append(json_text(i + 1))
          .append(", ")
          .append(args[n - 1])
          .append(")");
    }
    return "json(" + result + ")";
  }
  if (name == "JSON_OBJECT") {
    if (n == 2 && type(0)->IsArray()) {
      return "json(bq_json_object(" + json_text(0) + ", " + json_text(1) + "))";
    }
    if (n % 2 != 0) {
      return std::nullopt;
    }
    std::vector<std::string> keys;
    std::vector<std::string> values;
    for (size_t i = 0; i < n; i += 2) {
      keys.push_back(args[i]);
      values.push_back(args[i + 1]);
    }
    return "json(bq_json_object(CAST(json_array(" + Join(keys, ", ") +
           ") AS VARCHAR), CAST(json_array(" + Join(values, ", ") + ") AS VARCHAR)))";
  }
  if (name == "ARRAY_CONCAT" && n >= 1) {
    if (n == 1) {
      return args[0];
    }
    // BigQuery returns NULL when any array is NULL, where DuckDB skips it.
    std::vector<std::string> fields;
    std::vector<std::string> nulls;
    std::vector<std::string> lists;
    for (size_t i = 0; i < n; ++i) {
      const std::string field = "a" + std::to_string(i);
      fields.push_back(field + " := " + args[i]);
      nulls.push_back("_cat." + field + " IS NULL");
      lists.push_back("_cat." + field);
    }
    return "list_transform([struct_pack(" + Join(fields, ", ") + ")], _cat -> CASE WHEN " +
           Join(nulls, " OR ") + " THEN NULL ELSE list_concat(" + Join(lists, ", ") + ") END)[1]";
  }
  return TranslateFunction(name, arguments);
}

}  // namespace

// A JSONPath key as DuckDB spells it. Quoting every key keeps DuckDB's wildcards and other
// extensions from applying. DuckDB rejects the empty key.
std::optional<std::string> JsonPathKey(std::string_view key) {
  if (key.empty()) {
    return std::nullopt;
  }
  std::string quoted = ".\"";
  for (const char c : key) {
    if (c == '"' || c == '\\') {
      quoted += '\\';
    }
    quoted += c;
  }
  return quoted + "\"";
}

std::optional<std::string> Function(const googlesql::ResolvedFunctionCall& call, const Scope& scope,
                                    const Columns& columns) {
  const std::string name = ToUpperAscii(call.function()->Name());
  if (!call.generic_argument_list().empty() || !call.hint_list().empty() ||
      !call.collation_list().empty()) {
    return Unsupported(scope, "function " + name + " with generic arguments, hints or collation");
  }
  // CONTAINS_SUBSTR is supplied by our catalog because GoogleSQL lacks this BigQuery builtin.
  if (!call.function()->IsGoogleSQLBuiltin() && name != "CONTAINS_SUBSTR") {
    return Unsupported(scope, "function " + name);
  }
  // SAFE_ADD and its siblings are the arithmetic operators with the SAFE. prefix.
  static const std::map<std::string, std::string> safe_operators = {
      {"SAFE_ADD", "$ADD"},
      {"SAFE_SUBTRACT", "$SUBTRACT"},
      {"SAFE_MULTIPLY", "$MULTIPLY"},
      {"SAFE_NEGATE", "$UNARY_MINUS"}};
  const auto safe_operator = safe_operators.find(name);
  const std::string function = safe_operator == safe_operators.end() ? name : safe_operator->second;
  const bool safe = call.error_mode() == googlesql::ResolvedFunctionCallBase::SAFE_ERROR_MODE ||
                    safe_operator != safe_operators.end();
  if (!safe && call.error_mode() != googlesql::ResolvedFunctionCallBase::DEFAULT_ERROR_MODE) {
    return Unsupported(scope, "function " + name + " error mode");
  }
  const bool bucket =
      name == "DATE_BUCKET" || name == "DATETIME_BUCKET" || name == "TIMESTAMP_BUCKET";
  std::vector<std::string> args;
  for (const auto& argument : call.argument_list()) {
    // The bucket width INTERVAL becomes a plain count, since DuckDB intervals keep no single
    // part to count in.
    if (bucket && argument->type()->IsInterval()) {
      const auto width = BucketWidthOf(*argument);
      if (!width) {
        return Unsupported(scope, "function " + name + " bucket width");
      }
      if (width->count == nullptr) {
        args.push_back(std::to_string(width->factor));
        continue;
      }
      const auto count = Expression(*width->count, scope, columns);
      if (!count) {
        return std::nullopt;
      }
      args.push_back("(" + *count + " * " + std::to_string(width->factor) + ")");
      continue;
    }
    if (argument->type()->IsEnum()) {
      if (const auto mode = RoundingMode(*argument)) {
        args.push_back(QuoteLiteral(*mode));
        continue;
      }
      const auto part = DatePart(*argument);
      if (!part) {
        return Unsupported(scope, "function " + name + " date part");
      }
      args.push_back(QuoteLiteral(*part));
      continue;
    }
    const auto sql = Expression(*argument, scope, columns);
    if (!sql) {
      return std::nullopt;
    }
    args.push_back(*sql);
  }
  if (!safe) {
    auto sql = Call(call, function, args);
    if (!sql) {
      return Unsupported(scope, "function " + name);
    }
    return sql;
  }
  // SAFE. turns errors of the function itself into NULL, while errors evaluating its
  // arguments still propagate. The arguments are bound outside the lambda so TRY only covers
  // the call; binding them also keeps DuckDB from raising constant errors at bind time.
  // DuckDB's TRY rejects volatile functions, and error() is how translations raise errors.
  static const std::set<std::string> volatile_functions = {
      "RAND",         "GENERATE_UUID",    "CURRENT_TIMESTAMP",
      "CURRENT_DATE", "CURRENT_DATETIME", "CURRENT_TIME"};
  if (volatile_functions.contains(name)) {
    return Unsupported(scope, "SAFE." + name);
  }
  const std::string lambda = scope.context.FreshName("_s");
  std::vector<std::string> bound;
  std::vector<std::string> placeholders;
  for (size_t i = 0; i < args.size(); ++i) {
    if (call.argument_list(static_cast<int>(i))->type()->IsEnum()) {
      placeholders.push_back(args[i]);
      continue;
    }
    const std::string field = "a" + std::to_string(i + 1);
    bound.push_back(field + " := " + args[i]);
    placeholders.push_back(lambda);
    placeholders.back() += "." + field;
  }
  // The error() calls there are the function's own errors, which become NULL.
  const auto translated = Call(call, function, placeholders);
  const auto sql = translated ? WithoutErrors(*translated) : std::nullopt;
  if (!sql) {
    return Unsupported(scope, "SAFE." + name);
  }
  if (bound.empty()) {
    return "TRY(" + *sql + ")";
  }
  return "list_transform([struct_pack(" + Join(bound, ", ") + ")], " + lambda + " -> TRY(" + *sql +
         "))[1]";
}

}  // namespace bigquery_emulator_duckdb::translator
