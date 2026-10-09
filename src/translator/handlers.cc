#include "src/translator/handlers.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "googlesql/public/function.h"
#include "googlesql/public/type.h"
#include "googlesql/public/value.h"
#include "googlesql/resolved_ast/resolved_ast.h"
#include "nlohmann/json.hpp"
#include "src/duckdb_sql.h"
#include "src/translator/context.h"
#include "src/translator/functions.h"
#include "src/type_mapping.h"

namespace bigquery_emulator_duckdb::translator {

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
      "second", "millisecond", "microsecond", "isoyear", "isoweek", "dayofweek", "dayofyear",
  };
  return supported.contains(part) ? std::optional<std::string>(part) : std::nullopt;
}

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
        {"microsecond", {"micros", 1}},
    };
    const auto part = DatePart(*call->argument_list(1));
    const auto unit = part ? units.find(*part) : units.end();
    if (unit == units.end()) {
      return std::nullopt;
    }
    return BucketWidth{
        .unit = unit->second.first,
        .count = call->argument_list(0),
        .factor = unit->second.second,
    };
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
  for (const auto& [unit, count] : {
           std::pair<std::string, int64_t>{"months", value.interval_value().get_months()},
           {"days", value.interval_value().get_days()},
           {"micros", value.interval_value().get_micros()},
       }) {
    if (count != 0) {
      parts.push_back({.unit = unit, .factor = count});
    }
  }
  if (parts.size() > 1) {
    return std::nullopt;
  }
  return parts.empty() ? BucketWidth{.unit = "days", .factor = 0} : parts.at(0);
}

namespace {

// The translated arguments of a call.
std::vector<std::string> Sqls(const ScalarCall& call) {
  std::vector<std::string> sqls;
  sqls.reserve(call.arguments.size());
  std::ranges::transform(call.arguments, std::back_inserter(sqls), &FunctionArgument::sql);
  return sqls;
}

const googlesql::Type* TypeOf(const ScalarCall& call, size_t i) {
  return call.resolved.argument_list(static_cast<int>(i))->type();
}

}  // namespace

std::optional<std::string> MakeArray(const ScalarCall& call) {
  return "[" + Join(Sqls(call), ", ") + "]";
}

std::optional<std::string> Logical(const ScalarCall& call) {
  if (call.arguments.size() < 2) {
    return std::nullopt;
  }
  return "(" + Join(Sqls(call), call.name == "$AND" ? " AND " : " OR ") + ")";
}

std::optional<std::string> InList(const ScalarCall& call) {
  const std::vector<std::string> args = Sqls(call);
  if (args.size() < 2) {
    return std::nullopt;
  }
  const std::vector<std::string> values(args.begin() + 1, args.end());
  return "(" + args.at(0) + " IN (" + Join(values, ", ") + "))";
}

std::optional<std::string> Case(const ScalarCall& call) {
  const std::vector<std::string> args = Sqls(call);
  const size_t n = args.size();
  const size_t start = call.name == "$CASE_WITH_VALUE" ? 1 : 0;
  if (n < start + 3 || (n - start) % 2 != 1) {
    return std::nullopt;
  }
  std::string sql = "CASE ";
  if (start == 1) {
    sql += args.at(0) + " ";
  }
  for (size_t i = start; i + 1 < n; i += 2) {
    sql += "WHEN " + args.at(i) + " THEN " + args.at(i + 1) + " ";
  }
  return "(" + sql + "ELSE " + args.back() + " END)";
}

// The width is already a count of months, days or microseconds, see BucketWidthOf. Months count
// from the origin in the calendar, while days and microseconds are fixed lengths; TIMESTAMP and
// DATETIME take a day as 24 hours.
std::optional<std::string> Bucket(const ScalarCall& call) {
  const std::vector<std::string> args = Sqls(call);
  const size_t n = args.size();
  if (n != 2 && n != 3) {
    return std::nullopt;
  }
  const auto width = BucketWidthOf(*call.resolved.argument_list(1));
  const googlesql::Type* input = TypeOf(call, 0);
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
  const std::string origin = n == 3                ? args.at(2)
                             : input->IsDate()     ? "DATE '1950-01-01'"
                             : input->IsDatetime() ? "TIMESTAMP '1950-01-01 00:00:00'"
                                                   : "TIMESTAMPTZ '1950-01-01 00:00:00+00'";
  const std::string zero = input->IsTimestamp()
                               ? "'Zero bucket width INTERVAL is not allowed'"
                               : "'Exactly one non-zero INTERVAL part in bucket width is required'";
  return "list_transform([struct_pack(x := " + args.at(0) + ", w := " + args.at(1) +
         ", o := " + origin + ")], _bk -> CASE WHEN _bk.w < 0 THEN " +
         call.Raise("'Negative bucket width INTERVAL is not allowed'") + " WHEN _bk.w = 0 THEN " +
         call.Raise(zero) + " ELSE " + bucket + " END)[1]";
}

namespace {

// Original struct names in depth-first order. JSON semantics depend on names, including empty
// and duplicate ones, while DuckDB may hold these fields under positional internal names.
void JsonFieldNames(const googlesql::Type* type, std::vector<std::string>& names) {
  if (type->IsArray()) {
    JsonFieldNames(type->AsArray()->element_type(), names);
  } else if (type->IsStruct()) {
    for (const auto& field : type->AsStruct()->fields()) {
      names.push_back(field.name);
      JsonFieldNames(field.type, names);
    }
  }
}

// Pair each value with its original struct names, without evaluating the value twice. The
// JSON backend reconstructs the GoogleSQL type from these names and the physical value type.
std::optional<std::string> JsonArgument(const ScalarCall& call, size_t i) {
  const auto type = DuckDbType(TypeOf(call, i));
  if (!type) {
    return std::nullopt;
  }
  std::vector<std::string> names;
  JsonFieldNames(TypeOf(call, i), names);
  return "struct_pack(value := CAST(" + call.arguments.at(i).sql + " AS " + *type +
         "), names := " + QuoteLiteral(nlohmann::json(names).dump()) + ")";
}

// The JSON arguments of a call, or nullopt when one has a type DuckDB cannot hold.
std::optional<std::vector<std::string>> JsonArguments(const ScalarCall& call) {
  std::vector<std::string> arguments;
  for (size_t i = 0; i < call.arguments.size(); ++i) {
    const auto argument = JsonArgument(call, i);
    if (!argument) {
      return std::nullopt;
    }
    arguments.push_back(*argument);
  }
  return arguments;
}

}  // namespace

// TO_JSON(value, stringify_wide_numbers) and TO_JSON_STRING(value, pretty_print).
std::optional<std::string> ToJson(const ScalarCall& call) {
  const size_t n = call.arguments.size();
  const auto value = n == 1 || n == 2 ? JsonArgument(call, 0) : std::nullopt;
  if (!value) {
    return std::nullopt;
  }
  const std::string flag = n == 2 ? call.arguments.at(1).sql : "false";
  if (call.name == "TO_JSON_STRING") {
    return "bq_to_json_string(" + *value + ", " + flag + ")";
  }
  return "json(bq_to_json(" + *value + ", " + flag + "))";
}

// JSON_REMOVE and JSON_SET take the paths one by one, in order.
std::optional<std::string> JsonRemove(const ScalarCall& call) {
  const std::vector<std::string> args = Sqls(call);
  if (args.size() < 2) {
    return std::nullopt;
  }
  std::string result = "CAST(" + args.at(0) + " AS VARCHAR)";
  for (size_t i = 1; i < args.size(); ++i) {
    result.insert(0, "bq_json_remove(").append(", ").append(args.at(i)).append(")");
  }
  return "json(" + result + ")";
}

std::optional<std::string> JsonSet(const ScalarCall& call) {
  const std::vector<std::string> args = Sqls(call);
  const size_t n = args.size();
  if (n < 4 || n % 2 != 0) {
    return std::nullopt;
  }
  std::string result = "CAST(" + args.at(0) + " AS VARCHAR)";
  for (size_t i = 1; i + 1 < n; i += 2) {
    const auto value = JsonArgument(call, i + 1);
    if (!value) {
      return std::nullopt;
    }
    result.insert(0, "bq_json_set(")
        .append(", ")
        .append(args.at(i))
        .append(", ")
        .append(*value)
        .append(", ")
        .append(args.at(n - 1))
        .append(")");
  }
  return "json(" + result + ")";
}

// JSON_ARRAY_APPEND and JSON_ARRAY_INSERT pass every pair to one backend call so that all
// paths are validated before any mutation. Each argument occurs once, including the flag.
std::optional<std::string> JsonArrayModify(const ScalarCall& call) {
  const size_t n = call.arguments.size();
  if (n < 4 || n % 2 != 0) {
    return std::nullopt;
  }
  std::vector<std::string> args = {"CAST(" + call.arguments.at(0).sql + " AS VARCHAR)"};
  for (size_t i = 1; i + 1 < n; i += 2) {
    const auto value = JsonArgument(call, i + 1);
    if (!value) {
      return std::nullopt;
    }
    args.push_back("CAST(" + call.arguments.at(i).sql + " AS VARCHAR)");
    args.push_back(*value);
  }
  args.push_back("CAST(" + call.arguments.at(n - 1).sql + " AS BOOLEAN)");
  const std::string function =
      call.name == "JSON_ARRAY_INSERT" ? "bq_json_array_insert" : "bq_json_array_append";
  return "json(" + function + "(" + Join(args, ", ") + "))";
}

// JSON_ARRAY(values...).
std::optional<std::string> JsonArray(const ScalarCall& call) {
  const auto args = JsonArguments(call);
  if (!args) {
    return std::nullopt;
  }
  return args->empty() ? "JSON '[]'" : "json(bq_json_array(" + Join(*args, ", ") + "))";
}

// JSON_OBJECT(key, value, ...) and JSON_OBJECT(keys, values).
std::optional<std::string> JsonObject(const ScalarCall& call) {
  const auto args = JsonArguments(call);
  if (!args || args->size() % 2 != 0) {
    return std::nullopt;
  }
  return args->empty() ? "JSON '{}'" : "json(bq_json_object(" + Join(*args, ", ") + "))";
}

std::optional<std::string> ArrayConcat(const ScalarCall& call) {
  const std::vector<std::string> args = Sqls(call);
  const size_t n = args.size();
  if (n == 0) {
    return std::nullopt;
  }
  if (n == 1) {
    return args.at(0);
  }
  // BigQuery returns NULL when any array is NULL, where DuckDB skips it.
  std::vector<std::string> fields;
  std::vector<std::string> nulls;
  std::vector<std::string> lists;
  for (size_t i = 0; i < n; ++i) {
    const std::string field = "a" + std::to_string(i);
    fields.push_back(field + " := " + args.at(i));
    nulls.push_back("_cat." + field + " IS NULL");
    lists.push_back("_cat." + field);
  }
  return "list_transform([struct_pack(" + Join(fields, ", ") + ")], _cat -> CASE WHEN " +
         Join(nulls, " OR ") + " THEN NULL ELSE list_concat(" + Join(lists, ", ") + ") END)[1]";
}

// DuckDB's concat() skips NULL, where BigQuery returns NULL; its || operator does not.
std::optional<std::string> ConcatStrings(const ScalarCall& call) {
  if (call.arguments.empty()) {
    return std::nullopt;
  }
  return "(" + Join(Sqls(call), " || ") + ")";
}

// FORMAT goes to GoogleSQL's implementation, which src/backend_functions.cc registers to tell
// the type of each value by its DuckDB type. The values are cast to the DuckDB type that stands
// for their type; a value of another type, such as NUMERIC, is unsupported.
std::optional<std::string> Format(const ScalarCall& call) {
  static const std::map<googlesql::TypeKind, std::string_view> types = {
      {googlesql::TYPE_STRING, "VARCHAR"},
      {googlesql::TYPE_BYTES, "BLOB"},
      {googlesql::TYPE_INT64, "BIGINT"},
      {googlesql::TYPE_DOUBLE, "DOUBLE"},
      {googlesql::TYPE_BOOL, "BOOLEAN"},
      {googlesql::TYPE_DATE, "DATE"},
      {googlesql::TYPE_TIME, "TIME"},
      {googlesql::TYPE_DATETIME, "TIMESTAMP"},
      {googlesql::TYPE_TIMESTAMP, "TIMESTAMPTZ"},
      {googlesql::TYPE_BIGNUMERIC, "BIGNUM"},
  };
  std::vector<std::string> arguments;
  for (const FunctionArgument& argument : call.arguments) {
    const auto type = types.find(argument.type);
    if (type == types.end()) {
      return std::nullopt;
    }
    arguments.push_back("CAST(" + argument.sql + " AS " + std::string(type->second) + ")");
  }
  return "bq_format(" + Join(arguments, ", ") + ")";
}

// GREATEST and LEAST are NULL when any argument is, where DuckDB's skip NULL, and NaN when any
// argument is, which DuckDB orders above every other number and so only GREATEST gets right.
std::optional<std::string> Extremum(const ScalarCall& call) {
  if (call.arguments.empty()) {
    return std::nullopt;
  }
  const bool greatest = call.name == "GREATEST";
  std::string body = "CASE WHEN list_count(_ext) < len(_ext) THEN NULL ";
  if (!greatest && call.resolved.type()->IsDouble()) {
    body += "WHEN isnan(list_max(_ext)) THEN list_max(_ext) ";
  }
  body += std::string("ELSE ") + (greatest ? "list_max" : "list_min") + "(_ext) END";
  return "list_transform([[" + Join(Sqls(call), ", ") + "]], _ext -> " + body + ")[1]";
}

}  // namespace bigquery_emulator_duckdb::translator
