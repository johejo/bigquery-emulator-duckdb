#include "src/translator/functions_backend.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "src/translator/function_rule_builders.h"
#include "src/translator/functions.h"

namespace bigquery_emulator_duckdb::translator {
namespace {

using enum googlesql::TypeKind;

// SUBSTR and SUBSTRING. BigQuery starts at the first character for a position of 0 or one
// before the start, where DuckDB takes as many characters fewer. BYTES go to GoogleSQL, without
// a length taking the rest.
std::vector<Rule> Substr() {
  const std::string start =
      "CASE WHEN $2 > 0 THEN $2 WHEN $2 = 0 OR $2 < -length($1) THEN 1 ELSE length($1) + $2 + 1 "
      "END";
  return {
      {.arity = 2, .spelling = "substr($1, " + start + ")", .conditions = {Is(1, {TYPE_STRING})}},
      {
          .arity = 3,
          .spelling = "CASE WHEN $3 < 0 THEN !1 ELSE substr($1, " + start + ", $3) END",
          .conditions = {Is(1, {TYPE_STRING})},
          .defaults = {},
          .errors = {"'Third argument in SUBSTR() cannot be negative'"},
      },
      {
          .arity = {2, 3},
          .spelling = "bq_substr_bytes($1, $2, $3)",
          .conditions = {Is(1, {TYPE_BYTES})},
          .defaults = {"9223372036854775807"},
      },
  };
}

// FORMAT_DATE, FORMAT_DATETIME and FORMAT_TIMESTAMP, by the type of the value. A TIMESTAMP is
// formatted in the given time zone, by default UTC.
std::vector<Rule> FormatDateTime() {
  return {
      {.arity = 2, .spelling = "bq_format_date($1, $2)", .conditions = {Is(2, {TYPE_DATE})}},
      {
          .arity = 2,
          .spelling = "bq_format_datetime($1, $2)",
          .conditions = {Is(2, {TYPE_DATETIME})},
      },
      {
          .arity = {2, 3},
          .spelling = "bq_format_timestamp($1, $2, $3)",
          .conditions = {Is(2, {TYPE_TIMESTAMP})},
          .defaults = {"'UTC'"},
      },
  };
}

// A FLOAT64 function of `arity` arguments that src/backend_functions.cc registers.
std::vector<Rule> Float64(std::string_view function, std::size_t arity = 1) {
  return Same(function, arity, {Is(1, {TYPE_DOUBLE})});
}

// JSON_QUERY and the other extractions of `function` in src/backend_functions.cc, of a STRING
// or of JSON. `standard` is false for the legacy JSON_EXTRACT functions' JSONPath.
std::vector<Rule> JsonExtract(std::string_view function, bool standard, bool json_result) {
  const std::string flag = standard ? "true" : "false";
  const std::string of_json =
      std::string(function) + "_json(CAST($1 AS VARCHAR), $2, " + flag + ")";
  return {
      {
          .arity = {1, 2},
          .spelling = json_result ? "json(" + of_json + ")" : of_json,
          .conditions = {Is(1, {TYPE_JSON})},
          .defaults = {"'$'"},
      },
      {
          .arity = {1, 2},
          .spelling = std::string(function) + "($1, $2, " + flag + ")",
          .conditions = {Is(1, {TYPE_STRING})},
          .defaults = {"'$'"},
      },
  };
}

// A function that src/backend_functions.cc registers as `function` for STRING and as
// `function`_bytes for BYTES.
std::vector<Rule> Strings(std::string_view function, Arity arity) {
  return Concat({
      Same(function, arity, {Is(1, {TYPE_STRING})}),
      Same(std::string(function) + "_bytes", arity, {Is(1, {TYPE_BYTES})}),
  });
}

// A function that src/backend_functions.cc registers as bq_`name` for FLOAT64, as
// bq_`name`_numeric for NUMERIC and as bq_bignumeric_`name` for BIGNUMERIC, the last two of which
// keep their precision rather than going through FLOAT64.
std::vector<Rule> Numbers(std::string_view name, std::size_t arity = 1) {
  const std::string function = "bq_" + std::string(name);
  return Concat({
      {BigNumericOperator("bq_bignumeric_" + std::string(name), arity)},
      Float64(function, arity),
      Same(function + "_numeric", arity, {Is(1, {TYPE_NUMERIC})}),
  });
}

// TRIM, LTRIM and RTRIM. Without the characters to trim, they trim Unicode whitespace, where
// DuckDB's trim() only trims spaces.
std::vector<Rule> Trim(const std::string& function) {
  return {
      {.arity = 1, .spelling = function + "($1)", .conditions = {Is(1, {TYPE_STRING})}},
      {.arity = 2, .spelling = function + "_chars($1, $2)", .conditions = {Is(1, {TYPE_STRING})}},
      {.arity = 2, .spelling = function + "_bytes($1, $2)", .conditions = {Is(1, {TYPE_BYTES})}},
  };
}

// LPAD and RPAD, which pad with spaces by default.
std::vector<Rule> Pad(const std::string& function) {
  return {
      {
          .arity = {2, 3},
          .spelling = function + "($1, $2, $3)",
          .conditions = {Is(1, {TYPE_STRING})},
          .defaults = {"' '"},
      },
      {
          .arity = {2, 3},
          .spelling = function + "_bytes($1, $2, $3)",
          .conditions = {Is(1, {TYPE_BYTES})},
          .defaults = {"encode(' ')"},
      },
  };
}

}  // namespace

// Functions implemented by templates that call GoogleSQL's own implementations, which
// src/backend_functions.cc registers as bq_* DuckDB functions.
const std::unordered_map<std::string_view, std::vector<Rule>>& BackendRules() {
  static const auto* const kRules = new std::unordered_map<std::string_view, std::vector<Rule>>{
      // Construct and normalize separate interval components through GoogleSQL.
      {
          "$INTERVAL",
          {
              {
                  .arity = 2,
                  .spelling = "bq_interval($1, upper(#2))",
                  .conditions =
                      {
                          Part(2,
                               {
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
                               }),
                      },
              },
          },
      },
      {"MAKE_INTERVAL", {{.arity = 6, .spelling = "bq_make_interval($1, $2, $3, $4, $5, $6)"}}},
      {"JUSTIFY_HOURS", {{.arity = 1, .spelling = "bq_justify_hours($1)"}}},
      {"JUSTIFY_DAYS", {{.arity = 1, .spelling = "bq_justify_days($1)"}}},
      {"JUSTIFY_INTERVAL", {{.arity = 1, .spelling = "bq_justify_interval($1)"}}},
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
      {"PARSE_NUMERIC", {{.arity = 1, .spelling = "bq_parse_numeric($1)"}}},
      {"PARSE_BIGNUMERIC", {{.arity = 1, .spelling = "CAST(bq_parse_bignumeric($1) AS BIGNUM)"}}},
      {"SIN", Float64("bq_sin")},
      {"COS", Float64("bq_cos")},
      {"TAN", Float64("bq_tan")},
      {"ASIN", Float64("bq_asin")},
      {"ACOS", Float64("bq_acos")},
      {"ATAN", Float64("bq_atan")},
      {"ATAN2", Float64("bq_atan2", 2)},
      {"ASINH", Float64("bq_asinh")},
      {"TANH", Float64("bq_tanh")},
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
      {"COSINE_DISTANCE", {{.arity = 2, .spelling = "bq_cosine_distance($1, $2)"}}},
      {"EUCLIDEAN_DISTANCE", {{.arity = 2, .spelling = "bq_euclidean_distance($1, $2)"}}},
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
      {
          "INSTR",
          {
              {
                  .arity = {2, 4},
                  .spelling = "bq_instr($1, $2, $3, $4)",
                  .conditions = {Is(1, {TYPE_STRING})},
                  .defaults = {"1", "1"},
              },
              {
                  .arity = {2, 4},
                  .spelling = "bq_instr_bytes($1, $2, $3, $4)",
                  .conditions = {Is(1, {TYPE_BYTES})},
                  .defaults = {"1", "1"},
              },
          },
      },
      {
          "STRPOS",
          {
              {.arity = 2, .spelling = "strpos($1, $2)", .conditions = {Is(1, {TYPE_STRING})}},
              {
                  .arity = 2,
                  .spelling = "bq_instr_bytes($1, $2, 1, 1)",
                  .conditions = {Is(1, {TYPE_BYTES})},
              },
          },
      },
      {
          "STARTS_WITH",
          {
              {.arity = 2, .spelling = "starts_with($1, $2)", .conditions = {Is(1, {TYPE_STRING})}},
              {
                  .arity = 2,
                  .spelling = "bq_starts_with_bytes($1, $2)",
                  .conditions = {Is(1, {TYPE_BYTES})},
              },
          },
      },
      {
          "ENDS_WITH",
          {
              {.arity = 2, .spelling = "ends_with($1, $2)", .conditions = {Is(1, {TYPE_STRING})}},
              {
                  .arity = 2,
                  .spelling = "bq_ends_with_bytes($1, $2)",
                  .conditions = {Is(1, {TYPE_BYTES})},
              },
          },
      },
      {"SUBSTR", Substr()},
      {"SUBSTRING", Substr()},
      {
          "SPLIT",
          {
              {
                  .arity = {1, 2},
                  .spelling = "split($1, $2)",
                  .conditions = {Is(1, {TYPE_STRING})},
                  .defaults = {"','"},
              },
              {
                  .arity = 2,
                  .spelling = "bq_split_bytes($1, $2)",
                  .conditions = {Is(1, {TYPE_BYTES})},
              },
          },
      },
      // The mode is passed by its name.
      // Formatting and parsing dates and times. DuckDB's strftime() and strptime() differ in
      // the format elements, in how leniently they parse and in their errors. FORMAT_DATE,
      // FORMAT_DATETIME and FORMAT_TIMESTAMP each also take the other two types.
      {"FORMAT_DATE", FormatDateTime()},
      {"FORMAT_DATETIME", FormatDateTime()},
      {"FORMAT_TIMESTAMP", FormatDateTime()},
      {"FORMAT_TIME", {{.arity = 2, .spelling = "bq_format_time($1, $2)"}}},
      {"PARSE_DATE", {{.arity = 2, .spelling = "bq_parse_date($1, $2)"}}},
      {"PARSE_DATETIME", {{.arity = 2, .spelling = "bq_parse_datetime($1, $2)"}}},
      {"PARSE_TIME", {{.arity = 2, .spelling = "bq_parse_time($1, $2)"}}},
      // A string without a time zone is in the given one, by default UTC.
      {
          "PARSE_TIMESTAMP",
          {
              {
                  .arity = {2, 3},
                  .spelling = "bq_parse_timestamp($1, $2, $3)",
                  .conditions = {},
                  .defaults = {"'UTC'"},
              },
          },
      },
      {
          "NORMALIZE",
          {
              {
                  .arity = {1, 2},
                  .spelling = "bq_normalize($1, $2)",
                  .conditions = {},
                  .defaults = {"'NFC'"},
              },
          },
      },
      {
          "NORMALIZE_AND_CASEFOLD",
          {
              {
                  .arity = {1, 2},
                  .spelling = "bq_normalize_and_casefold($1, $2)",
                  .conditions = {},
                  .defaults = {"'NFC'"},
              },
          },
      },
      // BigQuery compares the NFKC normal forms, case folded. Only the STRING overload is
      // declared.
      {
          "CONTAINS_SUBSTR",
          {
              {
                  .arity = 2,
                  .spelling = "contains(bq_normalize_and_casefold($1, 'NFKC'), "
                              "bq_normalize_and_casefold($2, "
                              "'NFKC'))",
                  .conditions = {Is(1, {TYPE_STRING}), Is(2, {TYPE_STRING})},
              },
          },
      },
      // encode() takes a STRING's UTF-8 bytes.
      {
          "SHA512",
          {
              {
                  .arity = 1,
                  .spelling = "bq_sha512(encode($1))",
                  .conditions = {Is(1, {TYPE_STRING})},
              },
              {.arity = 1, .spelling = "bq_sha512($1)", .conditions = {Is(1, {TYPE_BYTES})}},
          },
      },
      // GoogleSQL leaves base32 out of its open source; src/backend_functions/string.cc
      // implements RFC 4648's.
      {"TO_BASE32", {{.arity = 1, .spelling = "bq_to_base32($1)"}}},
      {"FROM_BASE32", {{.arity = 1, .spelling = "bq_from_base32($1)"}}},
      {
          "FARM_FINGERPRINT",
          {
              {
                  .arity = 1,
                  .spelling = "bq_farm_fingerprint(encode($1))",
                  .conditions = {Is(1, {TYPE_STRING})},
              },
              {
                  .arity = 1,
                  .spelling = "bq_farm_fingerprint($1)",
                  .conditions = {Is(1, {TYPE_BYTES})},
              },
          },
      },
      {
          "INITCAP",
          {
              {.arity = 1, .spelling = "bq_initcap($1)"},
              {.arity = 2, .spelling = "bq_initcap_delimiters($1, $2)"},
          },
      },
      {"SOUNDEX", {{.arity = 1, .spelling = "bq_soundex($1)"}}},
      {
          "SAFE_CONVERT_BYTES_TO_STRING",
          {{.arity = 1, .spelling = "bq_safe_convert_bytes_to_string($1)"}},
      },
      // NET functions. HOST, REG_DOMAIN and PUBLIC_SUFFIX follow GoogleSQL's copy of the public
      // suffix list.
      {"IPV4_FROM_INT64", {{.arity = 1, .spelling = "bq_net_ipv4_from_int64($1)"}}},
      {"IPV4_TO_INT64", {{.arity = 1, .spelling = "bq_net_ipv4_to_int64($1)"}}},
      {"IP_FROM_STRING", {{.arity = 1, .spelling = "bq_net_ip_from_string($1)"}}},
      {"SAFE_IP_FROM_STRING", {{.arity = 1, .spelling = "bq_net_safe_ip_from_string($1)"}}},
      {"IP_TO_STRING", {{.arity = 1, .spelling = "bq_net_ip_to_string($1)"}}},
      {"IP_NET_MASK", {{.arity = 2, .spelling = "bq_net_ip_net_mask($1, $2)"}}},
      {"IP_TRUNC", {{.arity = 2, .spelling = "bq_net_ip_trunc($1, $2)"}}},
      {"HOST", {{.arity = 1, .spelling = "bq_net_host($1)"}}},
      {"REG_DOMAIN", {{.arity = 1, .spelling = "bq_net_reg_domain($1)"}}},
      {"PUBLIC_SUFFIX", {{.arity = 1, .spelling = "bq_net_public_suffix($1)"}}},
      // AEAD with Tink keysets of AES-GCM keys, where a STRING is its UTF-8 bytes. The keyset
      // chain of KEYS.KEYSET_CHAIN needs Cloud KMS.
      {
          "ENCRYPT",
          {
              {
                  .arity = 3,
                  .spelling = "bq_aead_encrypt($1, encode($2), encode($3))",
                  .conditions = {Is(1, {TYPE_BYTES}), Is(2, {TYPE_STRING})},
              },
              {
                  .arity = 3,
                  .spelling = "bq_aead_encrypt($1, $2, $3)",
                  .conditions = {Is(1, {TYPE_BYTES}), Is(2, {TYPE_BYTES})},
              },
          },
      },
      {
          "DECRYPT_BYTES",
          {
              {
                  .arity = 3,
                  .spelling = "bq_aead_decrypt_bytes($1, $2, $3)",
                  .conditions = {Is(1, {TYPE_BYTES})},
              },
          },
      },
      {
          "DECRYPT_STRING",
          {
              {
                  .arity = 3,
                  .spelling = "bq_aead_decrypt_string($1, $2, encode($3))",
                  .conditions = {Is(1, {TYPE_BYTES})},
              },
          },
      },
      // Without max_distance, the distance is not capped.
      {
          "EDIT_DISTANCE",
          {
              {
                  .arity = {2, 3},
                  .spelling = "bq_edit_distance($1, $2, $3)",
                  .conditions = {Is(1, {TYPE_STRING})},
                  .defaults = {"9223372036854775807"},
              },
              {
                  .arity = {2, 3},
                  .spelling = "bq_edit_distance_bytes($1, $2, $3)",
                  .conditions = {Is(1, {TYPE_BYTES})},
                  .defaults = {"9223372036854775807"},
              },
          },
      },
      // Regular expressions. DuckDB raises no error for a replacement that refers to a missing
      // group, and returns '' or NULL elements where a capturing group takes no part in a match.
      {"REGEXP_CONTAINS", Strings("bq_regexp_contains", 2)},
      {"REGEXP_REPLACE", Strings("bq_regexp_replace", 3)},
      {
          "REGEXP_EXTRACT",
          {
              {
                  .arity = {2, 4},
                  .spelling = "bq_regexp_extract($1, $2, $3, $4)",
                  .conditions = {Is(1, {TYPE_STRING})},
                  .defaults = {"1", "1"},
              },
              {
                  .arity = {2, 4},
                  .spelling = "bq_regexp_extract_bytes($1, $2, $3, $4)",
                  .conditions = {Is(1, {TYPE_BYTES})},
                  .defaults = {"1", "1"},
              },
          },
      },
      {"REGEXP_EXTRACT_ALL", Strings("bq_regexp_extract_all", 2)},
      {
          "REGEXP_INSTR",
          {
              {
                  .arity = {2, 5},
                  .spelling = "bq_regexp_instr($1, $2, $3, $4, $5)",
                  .conditions = {Is(1, {TYPE_STRING})},
                  .defaults = {"1", "1", "0"},
              },
              {
                  .arity = {2, 5},
                  .spelling = "bq_regexp_instr_bytes($1, $2, $3, $4, $5)",
                  .conditions = {Is(1, {TYPE_BYTES})},
                  .defaults = {"1", "1", "0"},
              },
          },
      },
      // JSON goes to GoogleSQL as its text; see src/backend_functions.cc.
      {
          "PARSE_JSON",
          {
              {
                  .arity = {1, 2},
                  .spelling = "json(bq_parse_json($1, $2))",
                  .conditions = {},
                  .defaults = {"'exact'"},
              },
          },
      },
      {
          "BOOL",
          {
              {
                  .arity = 1,
                  .spelling = "bq_json_bool(CAST($1 AS VARCHAR))",
                  .conditions = {Is(1, {TYPE_JSON})},
              },
          },
      },
      {
          "STRING",
          {
              {
                  .arity = 1,
                  .spelling = "bq_json_string(CAST($1 AS VARCHAR))",
                  .conditions = {Is(1, {TYPE_JSON})},
              },
              {
                  .arity = {1, 2},
                  .spelling = "bq_timestamp_string($1, $2)",
                  .conditions = {Is(1, {TYPE_TIMESTAMP})},
                  .defaults = {"'UTC'"},
              },
          },
      },
      {
          "INT64",
          {
              {
                  .arity = 1,
                  .spelling = "bq_json_int64(CAST($1 AS VARCHAR))",
                  .conditions = {Is(1, {TYPE_JSON})},
              },
          },
      },
      {
          "FLOAT64",
          {
              {
                  .arity = {1, 2},
                  .spelling = "bq_json_float64(CAST($1 AS VARCHAR), $2)",
                  .conditions = {Is(1, {TYPE_JSON})},
                  .defaults = {"'round'"},
              },
          },
      },
      {
          "DOUBLE",
          {
              {
                  .arity = {1, 2},
                  .spelling = "bq_json_float64(CAST($1 AS VARCHAR), $2)",
                  .conditions = {Is(1, {TYPE_JSON})},
                  .defaults = {"'round'"},
              },
          },
      },
      {"JSON_TYPE", {{.arity = 1, .spelling = "bq_json_type(CAST($1 AS VARCHAR))"}}},
      {
          "$SUBSCRIPT",
          {
              {
                  .arity = 2,
                  .spelling = "json(bq_json_field(CAST($1 AS VARCHAR), $2))",
                  .conditions = {Is(1, {TYPE_JSON}), Is(2, {TYPE_STRING})},
              },
              {
                  .arity = 2,
                  .spelling = "json(bq_json_element(CAST($1 AS VARCHAR), $2))",
                  .conditions = {Is(1, {TYPE_JSON}), Is(2, {TYPE_INT64})},
              },
          },
      },
      {
          "JSON_FLATTEN",
          {{.arity = 1, .spelling = "CAST(bq_json_flatten(CAST($1 AS VARCHAR)) AS JSON[])"}},
      },
      {
          "$SUBSCRIPT",
          {
              {
                  .arity = 2,
                  .spelling = "json(bq_json_field(CAST($1 AS VARCHAR), $2))",
                  .conditions = {Is(1, {TYPE_JSON}), Is(2, {TYPE_STRING})},
              },
              {
                  .arity = 2,
                  .spelling = "json(bq_json_element(CAST($1 AS VARCHAR), $2))",
                  .conditions = {Is(1, {TYPE_JSON}), Is(2, {TYPE_INT64})},
              },
          },
      },
      {"JSON_QUERY", JsonExtract("bq_json_query", true, true)},
      {"JSON_EXTRACT", JsonExtract("bq_json_query", false, true)},
      {"JSON_VALUE", JsonExtract("bq_json_value", true, false)},
      {"JSON_EXTRACT_SCALAR", JsonExtract("bq_json_value", false, false)},
      {"JSON_QUERY_ARRAY", JsonExtract("bq_json_query_array", true, false)},
      {"JSON_EXTRACT_ARRAY", JsonExtract("bq_json_query_array", false, false)},
      {"JSON_VALUE_ARRAY", JsonExtract("bq_json_value_array", true, false)},
      {"JSON_EXTRACT_STRING_ARRAY", JsonExtract("bq_json_value_array", false, false)},
      {
          "LAX_BOOL",
          {
              {
                  .arity = 1,
                  .spelling = "bq_lax_bool(CAST($1 AS VARCHAR))",
                  .conditions = {Is(1, {TYPE_JSON})},
              },
          },
      },
      {
          "LAX_INT64",
          {
              {
                  .arity = 1,
                  .spelling = "bq_lax_int64(CAST($1 AS VARCHAR))",
                  .conditions = {Is(1, {TYPE_JSON})},
              },
          },
      },
      {
          "LAX_FLOAT64",
          {
              {
                  .arity = 1,
                  .spelling = "bq_lax_float64(CAST($1 AS VARCHAR))",
                  .conditions = {Is(1, {TYPE_JSON})},
              },
          },
      },
      {
          "LAX_DOUBLE",
          {
              {
                  .arity = 1,
                  .spelling = "bq_lax_float64(CAST($1 AS VARCHAR))",
                  .conditions = {Is(1, {TYPE_JSON})},
              },
          },
      },
      {
          "LAX_STRING",
          {
              {
                  .arity = 1,
                  .spelling = "bq_lax_string(CAST($1 AS VARCHAR))",
                  .conditions = {Is(1, {TYPE_JSON})},
              },
          },
      },
      {
          "JSON_KEYS",
          {
              {
                  .arity = 3,
                  .spelling = "CAST(json(bq_json_keys(CAST($1 AS VARCHAR), $2, $3)) AS VARCHAR[])",
                  .conditions = {Is(1, {TYPE_JSON})},
              },
          },
      },
      {
          "JSON_STRIP_NULLS",
          {
              {
                  .arity = 4,
                  .spelling = "json(bq_json_strip_nulls(CAST($1 AS VARCHAR), $2, $3, $4))",
                  .conditions = {Is(1, {TYPE_JSON})},
              },
          },
      },
  };
  return *kRules;
}

}  // namespace bigquery_emulator_duckdb::translator
