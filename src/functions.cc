#include "src/functions.h"

#include <cstddef>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace bigquery_emulator_duckdb {
namespace {

// BigQuery functions that DuckDB has under another name, with the same arguments.
const std::unordered_map<std::string_view, std::string_view>& FunctionNames() {
  static const auto* const kNames = new std::unordered_map<std::string_view, std::string_view>{
      {"CONTAINS_SUBSTR", "contains"},
      {"DIV", "divide"},
      {"FORMAT", "printf"},
      {"GENERATE_ARRAY", "generate_series"},
      {"GENERATE_UUID", "uuid"},
      {"IS_INF", "isinf"},
      {"IS_NAN", "isnan"},
      {"JSON_EXTRACT_SCALAR", "json_extract_string"},
      {"JSON_QUERY", "json_extract"},
      {"PARSE_JSON", "json"},
      {"RAND", "random"},
      {"REGEXP_CONTAINS", "regexp_matches"},
      {"TIMESTAMP_SECONDS", "to_timestamp"},
      {"UNIX_MICROS", "epoch_us"},
      {"UNIX_MILLIS", "epoch_ms"},
  };
  return *kNames;
}

struct TemplateRule {
  std::size_t argument_count;
  std::string_view spelling;
};

// BigQuery functions that DuckDB spells with the arguments in another order, with an extra
// argument, or as an operator. Each rule matches one argument count, so a call that carries an
// argument the rule does not cover - a time zone, say - is left to the other translations.
const std::unordered_map<std::string_view, std::vector<TemplateRule>>& FunctionTemplates() {
  static const auto* const kTemplates =
      new std::unordered_map<std::string_view, std::vector<TemplateRule>>{
          // A division by zero is an error in BigQuery and +Inf in DuckDB, so SAFE_DIVIDE has
          // to make the zero itself disappear.
          {"SAFE_DIVIDE", {{2, "($1 / NULLIF($2, 0))"}}},
          // DuckDB's log() is the base 10 logarithm, BigQuery's LOG() the natural one, and the
          // two argument form takes the base first rather than last.
          {"LOG", {{1, "ln($1)"}, {2, "log($2, $1)"}}},

          // Date and time arithmetic: DuckDB uses the operators and puts the date part first,
          // as a string rather than as a keyword.
          {"DATE_ADD", {{2, "CAST($1 + $2 AS DATE)"}}},
          {"DATE_SUB", {{2, "CAST($1 - $2 AS DATE)"}}},
          {"DATETIME_ADD", {{2, "($1 + $2)"}}},
          {"DATETIME_SUB", {{2, "($1 - $2)"}}},
          {"TIMESTAMP_ADD", {{2, "($1 + $2)"}}},
          {"TIMESTAMP_SUB", {{2, "($1 - $2)"}}},
          {"TIME_ADD", {{2, "($1 + $2)"}}},
          {"TIME_SUB", {{2, "($1 - $2)"}}},
          {"DATE_DIFF", {{3, "date_diff(#3, $2, $1)"}}},
          {"DATETIME_DIFF", {{3, "date_diff(#3, $2, $1)"}}},
          {"TIMESTAMP_DIFF", {{3, "date_diff(#3, $2, $1)"}}},
          {"TIME_DIFF", {{3, "date_diff(#3, $2, $1)"}}},
          // date_trunc() widens a DATE to a TIMESTAMP, which DATE_TRUNC does not.
          {"DATE_TRUNC", {{2, "CAST(date_trunc(#2, $1) AS DATE)"}}},
          {"DATETIME_TRUNC", {{2, "date_trunc(#2, $1)"}}},
          {"TIMESTAMP_TRUNC", {{2, "date_trunc(#2, $1)"}}},

          // Formatting and parsing: DuckDB takes the value first and the format second.
          {"FORMAT_DATE", {{2, "strftime($2, $1)"}}},
          {"FORMAT_DATETIME", {{2, "strftime($2, $1)"}}},
          {"FORMAT_TIMESTAMP", {{2, "strftime($2, $1)"}}},
          {"PARSE_DATE", {{2, "CAST(strptime($2, $1) AS DATE)"}}},
          {"PARSE_DATETIME", {{2, "strptime($2, $1)"}}},
          {"PARSE_TIMESTAMP", {{2, "CAST(strptime($2, $1) AS TIMESTAMPTZ)"}}},

          // Epoch conversions. The DuckDB functions return a civil timestamp, which is read as
          // UTC to arrive at the instant BigQuery means.
          {"UNIX_SECONDS", {{1, "CAST(epoch($1) AS BIGINT)"}}},
          {"TIMESTAMP_MILLIS", {{1, "(epoch_ms($1) AT TIME ZONE 'UTC')"}}},
          {"TIMESTAMP_MICROS", {{1, "(make_timestamp($1) AT TIME ZONE 'UTC')"}}},
          {"DATE_FROM_UNIX_DATE",
           {{1, "CAST(DATE '1970-01-01' + to_days(CAST($1 AS INTEGER)) AS DATE)"}}},

          // REGEXP_REPLACE replaces every occurrence; DuckDB needs the global flag for that.
          {"REGEXP_REPLACE", {{3, "regexp_replace($1, $2, $3, 'g')"}}},
          // SPLIT defaults to a comma, DuckDB has no default.
          {"SPLIT", {{1, "split($1, ',')"}}},
          {"TO_JSON_STRING", {{1, "CAST(to_json($1) AS VARCHAR)"}}},
      };
  return *kTemplates;
}

}  // namespace

std::optional<std::string_view> DuckDbFunctionName(std::string_view upper_name) {
  const auto it = FunctionNames().find(upper_name);
  if (it == FunctionNames().end()) {
    return std::nullopt;
  }
  return it->second;
}

std::optional<std::string_view> DuckDbFunctionTemplate(std::string_view upper_name,
                                                       std::size_t argument_count) {
  const auto it = FunctionTemplates().find(upper_name);
  if (it == FunctionTemplates().end()) {
    return std::nullopt;
  }
  for (const TemplateRule& rule : it->second) {
    if (rule.argument_count == argument_count) {
      return rule.spelling;
    }
  }
  return std::nullopt;
}

}  // namespace bigquery_emulator_duckdb
