#include "src/translator/functions_backend.h"

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "src/duckdb_sql.h"
#include "src/function_catalog/rule_builders.h"
#include "src/function_catalog/rules.h"
#include "src/type_mapping.h"

namespace bigquery_emulator_duckdb::translator {

using function_catalog::Rule;

using function_catalog::Arity;
using function_catalog::BigNumericOperator;
using function_catalog::Concat;
using function_catalog::Is;
using function_catalog::Part;
using function_catalog::Same;
using function_catalog::StaticCondition;
using function_catalog::StaticRule;
using function_catalog::TypeUse;

namespace {

using enum googlesql::TypeKind;

// GoogleSQL splits the RANGE into the bounds of its subranges, each subrange's end the next one's
// start, which become the RANGEs, and fails on an UNBOUNDED end and on steps BigQuery rejects. The
// last, partial subrange is kept by default.
std::vector<Rule> GenerateRangeArray() {
  const auto bound = [](std::string_view name) {
    return "struct_extract($1, " + QuoteLiteral(name) + ")";
  };
  return {
      {
          .arity = {2, 3},
          .spelling = "list_transform([bq_generate_range_array(" + bound(kRangeStart) + ", " +
                      bound(kRangeEnd) +
                      ", $2, $3)], lambda _b: list_transform(range(1, len(_b)), " +
                      "lambda _i: struct_pack(" + QuoteIdentifier(kRangeStart) + " := _b[_i], " +
                      QuoteIdentifier(kRangeEnd) + " := _b[_i + 1])))[1]",
          .conditions = {},
          .defaults = {"true"},
      },
  };
}

// STRING and BYTES use GoogleSQL's position, length and NULL semantics. An omitted length
// takes the rest of the input.
constexpr googlesql::TypeKind kStringTypes[] = {TYPE_STRING};
constexpr googlesql::TypeKind kBytesTypes[] = {TYPE_BYTES};
constexpr StaticCondition kStringInput[] = {{.argument = 1, .types = kStringTypes}};
constexpr StaticCondition kBytesInput[] = {{.argument = 1, .types = kBytesTypes}};
constexpr std::string_view kSubstrDefaults[] = {"9223372036854775807"};
constexpr StaticRule kSubstr[] = {
    {
        .arity = {2, 3},
        .spelling = "bq_substr($1, $2, $3)",
        .conditions = kStringInput,
        .defaults = kSubstrDefaults,
    },
    {
        .arity = {2, 3},
        .spelling = "bq_substr_bytes($1, $2, $3)",
        .conditions = kBytesInput,
        .defaults = kSubstrDefaults,
    },
};

// FORMAT_DATE, FORMAT_DATETIME and FORMAT_TIMESTAMP, by the type of the value. A TIMESTAMP is
// formatted in the given time zone, by default UTC.
constexpr googlesql::TypeKind kDateTypes[] = {TYPE_DATE};
constexpr googlesql::TypeKind kDateTimeTypes[] = {TYPE_DATETIME};
constexpr googlesql::TypeKind kTimestampTypes[] = {TYPE_TIMESTAMP};
constexpr StaticCondition kDateValue[] = {{.argument = 2, .types = kDateTypes}};
constexpr StaticCondition kDateTimeValue[] = {{.argument = 2, .types = kDateTimeTypes}};
constexpr StaticCondition kTimestampValue[] = {{.argument = 2, .types = kTimestampTypes}};
constexpr std::string_view kUtcDefault[] = {"'UTC'"};
constexpr StaticRule kFormatDateTime[] = {
    {.arity = 2, .spelling = "bq_format_date($1, $2)", .conditions = kDateValue},
    {.arity = 2, .spelling = "bq_format_datetime($1, $2)", .conditions = kDateTimeValue},
    {
        .arity = {2, 3},
        .spelling = "bq_format_timestamp($1, $2, $3)",
        .conditions = kTimestampValue,
        .defaults = kUtcDefault,
    },
};

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

// CODE_POINTS_TO_STRING and CODE_POINTS_TO_BYTES, which are NULL when an element is NULL.
std::vector<Rule> CodePointsTo(const std::string& function) {
  return {
      {
          .arity = 1,
          .spelling = "CASE WHEN list_count($1) <> len($1) THEN NULL ELSE " + function + "($1) END",
      },
  };
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

std::span<const function_catalog::StaticNamedDefinition<StaticRule>> StaticBackendRules() {
  static constexpr function_catalog::StaticNamedDefinition<StaticRule> kRules[] = {
      {"SUBSTR", {.rules = kSubstr}},
      {"SUBSTRING", {.rules = kSubstr}},
      {"FORMAT_DATE", {.rules = kFormatDateTime}},
      {"FORMAT_DATETIME", {.rules = kFormatDateTime}},
      {"FORMAT_TIMESTAMP", {.rules = kFormatDateTime}},
  };
  return kRules;
}

// Functions implemented by templates that call GoogleSQL's own implementations, which
// src/backend_functions.cc registers as bq_* DuckDB functions.
std::span<const function_catalog::NamedDefinition<Rule>> BackendRules() {
  static const function_catalog::NamedDefinition<Rule> kRules[] =
      {
          // Construct and normalize separate interval components through GoogleSQL.
          {
              "$INTERVAL",
              {
                  .rules =
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
                  .uses = {TypeUse::kBuildsOrReadsInterval},
              },
          },
          {
              "MAKE_INTERVAL",
              {
                  .rules = {{.arity = 6, .spelling = "bq_make_interval($1, $2, $3, $4, $5, $6)"}},
                  .uses = {TypeUse::kBuildsOrReadsInterval},
              },
          },
          {
              "JUSTIFY_HOURS",
              {
                  .rules = {{.arity = 1, .spelling = "bq_justify_hours($1)"}},
                  .uses = {TypeUse::kBuildsOrReadsInterval},
              },
          },
          {
              "JUSTIFY_DAYS",
              {
                  .rules = {{.arity = 1, .spelling = "bq_justify_days($1)"}},
                  .uses = {TypeUse::kBuildsOrReadsInterval},
              },
          },
          {
              "JUSTIFY_INTERVAL",
              {
                  .rules = {{.arity = 1, .spelling = "bq_justify_interval($1)"}},
                  .uses = {TypeUse::kBuildsOrReadsInterval},
              },
          },
          // DuckDB returns infinities where BigQuery raises errors, such as EXP(1000).
          {"SQRT", {.rules = Numbers("sqrt"), .uses = {TypeUse::kBigNumeric}}},
          {"CBRT", {.rules = Numbers("cbrt"), .uses = {TypeUse::kBigNumeric}}},
          {"POW", {.rules = Numbers("pow", 2), .uses = {TypeUse::kBigNumeric}}},
          {"POWER", {.rules = Numbers("pow", 2), .uses = {TypeUse::kBigNumeric}}},
          {"EXP", {.rules = Numbers("exp"), .uses = {TypeUse::kBigNumeric}}},
          {"LN", {.rules = Numbers("ln"), .uses = {TypeUse::kBigNumeric}}},
          {"LOG10", {.rules = Numbers("log10"), .uses = {TypeUse::kBigNumeric}}},
          {
              "LOG",
              {.rules = Concat({Numbers("ln"), Numbers("log", 2)}), .uses = {TypeUse::kBigNumeric}},
          },
          {"PARSE_NUMERIC", {.rules = {{.arity = 1, .spelling = "bq_parse_numeric($1)"}}}},
          {
              "PARSE_BIGNUMERIC",
              {
                  .rules = {{.arity = 1, .spelling = "CAST(bq_parse_bignumeric($1) AS BIGNUM)"}},
                  .uses = {TypeUse::kBigNumeric},
              },
          },
          {"COSINE_DISTANCE", {.rules = {{.arity = 2, .spelling = "bq_cosine_distance($1, $2)"}}}},
          {
              "EUCLIDEAN_DISTANCE",
              {.rules = {{.arity = 2, .spelling = "bq_euclidean_distance($1, $2)"}}},
          },
          // Strings. DuckDB's BLOB has almost no functions, its case mapping is simple rather than
          // full, it reverses grapheme clusters rather than characters, it trims spaces rather than
          // whitespace, and it neither raises BigQuery's errors nor has its output limit. Where it
          // agrees, the STRING overload stays DuckDB's.
          {"REPEAT", {.rules = Strings("bq_repeat", 2)}},
          {"LOWER", {.rules = Strings("bq_lower", 1)}},
          {"UPPER", {.rules = Strings("bq_upper", 1)}},
          {"REVERSE", {.rules = Strings("bq_reverse", 1)}},
          {"REPLACE", {.rules = Strings("bq_replace", 3)}},
          {"TRANSLATE", {.rules = Strings("bq_translate", 3)}},
          {"ASCII", {.rules = Strings("bq_ascii", 1)}},
          {"UNICODE", {.rules = Same("bq_unicode", 1, {Is(1, {TYPE_STRING})})}},
          {"CHR", {.rules = {{.arity = 1, .spelling = "bq_chr($1)"}}}},
          {"TO_CODE_POINTS", {.rules = Strings("bq_to_code_points", 1)}},
          {"CODE_POINTS_TO_STRING", {.rules = CodePointsTo("bq_code_points_to_string")}},
          {"CODE_POINTS_TO_BYTES", {.rules = CodePointsTo("bq_code_points_to_bytes")}},
          {"LEFT", {.rules = Strings("bq_left", 2)}},
          {"RIGHT", {.rules = Strings("bq_right", 2)}},
          {"LPAD", {.rules = Pad("bq_lpad")}},
          {"RPAD", {.rules = Pad("bq_rpad")}},
          {"TRIM", {.rules = Trim("bq_trim")}},
          {"LTRIM", {.rules = Trim("bq_ltrim")}},
          {"RTRIM", {.rules = Trim("bq_rtrim")}},
          {
              "INSTR",
              {
                  .rules =
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
          },
          {
              "STRPOS",
              {
                  .rules =
                      {
                          {
                              .arity = 2,
                              .spelling = "strpos($1, $2)",
                              .conditions = {Is(1, {TYPE_STRING})},
                          },
                          {
                              .arity = 2,
                              .spelling = "bq_instr_bytes($1, $2, 1, 1)",
                              .conditions = {Is(1, {TYPE_BYTES})},
                          },
                      },
              },
          },
          {
              "STARTS_WITH",
              {
                  .rules =
                      {
                          {
                              .arity = 2,
                              .spelling = "starts_with($1, $2)",
                              .conditions = {Is(1, {TYPE_STRING})},
                          },
                          {
                              .arity = 2,
                              .spelling = "bq_starts_with_bytes($1, $2)",
                              .conditions = {Is(1, {TYPE_BYTES})},
                          },
                      },
              },
          },
          {
              "ENDS_WITH",
              {
                  .rules =
                      {
                          {
                              .arity = 2,
                              .spelling = "ends_with($1, $2)",
                              .conditions = {Is(1, {TYPE_STRING})},
                          },
                          {
                              .arity = 2,
                              .spelling = "bq_ends_with_bytes($1, $2)",
                              .conditions = {Is(1, {TYPE_BYTES})},
                          },
                      },
              },
          },

          {
              "SPLIT",
              {
                  .rules =
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
          },
          // GoogleSQL stops before an element that would overflow, and rejects a zero step and
          // arrays beyond BigQuery's limit.
          {
              "GENERATE_DATE_ARRAY",
              {
                  .rules =
                      {
                          {.arity = 2, .spelling = "bq_generate_date_array($1, $2, 1, 'day')"},
                          {.arity = 4, .spelling = "bq_generate_date_array($1, $2, $3, #4)"},
                      },
              },
          },
          {
              "GENERATE_RANGE_ARRAY",
              {
                  .rules = GenerateRangeArray(),
                  .uses = {TypeUse::kBuildsOrReadsInterval, TypeUse::kBuildsOrReadsRange},
              },
          },
          // The mode is passed by its name.
          // Formatting and parsing dates and times. DuckDB's strftime() and strptime() differ in
          // the format elements, in how leniently they parse and in their errors. FORMAT_DATE,
          // FORMAT_DATETIME and FORMAT_TIMESTAMP each also take the other two types.

          {"FORMAT_TIME", {.rules = {{.arity = 2, .spelling = "bq_format_time($1, $2)"}}}},
          {"PARSE_DATE", {.rules = {{.arity = 2, .spelling = "bq_parse_date($1, $2)"}}}},
          {"PARSE_DATETIME", {.rules = {{.arity = 2, .spelling = "bq_parse_datetime($1, $2)"}}}},
          {"PARSE_TIME", {.rules = {{.arity = 2, .spelling = "bq_parse_time($1, $2)"}}}},
          // A string without a time zone is in the given one, by default UTC.
          {
              "PARSE_TIMESTAMP",
              {
                  .rules =
                      {
                          {
                              .arity = {2, 3},
                              .spelling = "bq_parse_timestamp($1, $2, $3)",
                              .conditions = {},
                              .defaults = {"'UTC'"},
                          },
                      },
              },
          },
          {
              "NORMALIZE",
              {
                  .rules =
                      {
                          {
                              .arity = {1, 2},
                              .spelling = "bq_normalize($1, $2)",
                              .conditions = {},
                              .defaults = {"'NFC'"},
                          },
                      },
              },
          },
          {
              "NORMALIZE_AND_CASEFOLD",
              {
                  .rules =
                      {
                          {
                              .arity = {1, 2},
                              .spelling = "bq_normalize_and_casefold($1, $2)",
                              .conditions = {},
                              .defaults = {"'NFC'"},
                          },
                      },
              },
          },
          // encode() takes a STRING's UTF-8 bytes.
          {
              "SHA512",
              {
                  .rules =
                      {
                          {
                              .arity = 1,
                              .spelling = "bq_sha512(encode($1))",
                              .conditions = {Is(1, {TYPE_STRING})},
                          },
                          {
                              .arity = 1,
                              .spelling = "bq_sha512($1)",
                              .conditions = {Is(1, {TYPE_BYTES})},
                          },
                      },
              },
          },
          // GoogleSQL leaves base32 out of its open source;
          // src/googlesql_functions_duckdb/string.cc
          // implements RFC 4648's.
          {"TO_BASE32", {.rules = {{.arity = 1, .spelling = "bq_to_base32($1)"}}}},
          {"FROM_BASE32", {.rules = {{.arity = 1, .spelling = "bq_from_base32($1)"}}}},
          {
              "FARM_FINGERPRINT",
              {
                  .rules =
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
          },
          {
              "INITCAP",
              {
                  .rules =
                      {
                          {.arity = 1, .spelling = "bq_initcap($1)"},
                          {.arity = 2, .spelling = "bq_initcap_delimiters($1, $2)"},
                      },
              },
          },
          {"SOUNDEX", {.rules = {{.arity = 1, .spelling = "bq_soundex($1)"}}}},
          {
              "SAFE_CONVERT_BYTES_TO_STRING",
              {.rules = {{.arity = 1, .spelling = "bq_safe_convert_bytes_to_string($1)"}}},
          },
          // NET functions. HOST, REG_DOMAIN and PUBLIC_SUFFIX follow GoogleSQL's copy of the public
          // suffix list.
          {
              "NET.IPV4_FROM_INT64",
              {.rules = {{.arity = 1, .spelling = "bq_net_ipv4_from_int64($1)"}}},
          },
          {"NET.IPV4_TO_INT64", {.rules = {{.arity = 1, .spelling = "bq_net_ipv4_to_int64($1)"}}}},
          {
              "NET.IP_FROM_STRING",
              {.rules = {{.arity = 1, .spelling = "bq_net_ip_from_string($1)"}}},
          },
          {
              "NET.SAFE_IP_FROM_STRING",
              {.rules = {{.arity = 1, .spelling = "bq_net_safe_ip_from_string($1)"}}},
          },
          {"NET.IP_TO_STRING", {.rules = {{.arity = 1, .spelling = "bq_net_ip_to_string($1)"}}}},
          {"NET.IP_NET_MASK", {.rules = {{.arity = 2, .spelling = "bq_net_ip_net_mask($1, $2)"}}}},
          {"NET.IP_TRUNC", {.rules = {{.arity = 2, .spelling = "bq_net_ip_trunc($1, $2)"}}}},
          {"NET.HOST", {.rules = {{.arity = 1, .spelling = "bq_net_host($1)"}}}},
          {"NET.REG_DOMAIN", {.rules = {{.arity = 1, .spelling = "bq_net_reg_domain($1)"}}}},
          {"NET.PUBLIC_SUFFIX", {.rules = {{.arity = 1, .spelling = "bq_net_public_suffix($1)"}}}},
          // AEAD with Tink keysets of AES-GCM keys, and deterministic AEAD with AES-SIV keys, where
          // a
          // STRING is its UTF-8 bytes. The keyset chain of KEYS.KEYSET_CHAIN needs Cloud KMS, as do
          // the KEYS functions of wrapped keysets.
          {
              "AEAD.ENCRYPT",
              {
                  .rules =
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
          },
          {
              "AEAD.DECRYPT_BYTES",
              {
                  .rules =
                      {
                          {
                              .arity = 3,
                              .spelling = "bq_aead_decrypt_bytes($1, $2, $3)",
                              .conditions = {Is(1, {TYPE_BYTES})},
                          },
                      },
              },
          },
          {
              "AEAD.DECRYPT_STRING",
              {
                  .rules =
                      {
                          {
                              .arity = 3,
                              .spelling = "bq_aead_decrypt_string($1, $2, encode($3))",
                              .conditions = {Is(1, {TYPE_BYTES})},
                          },
                      },
              },
          },
          {
              "DETERMINISTIC_ENCRYPT",
              {
                  .rules =
                      {
                          {
                              .arity = 3,
                              .spelling = "bq_deterministic_encrypt($1, encode($2), encode($3))",
                              .conditions = {Is(1, {TYPE_BYTES}), Is(2, {TYPE_STRING})},
                          },
                          {
                              .arity = 3,
                              .spelling = "bq_deterministic_encrypt($1, $2, $3)",
                              .conditions = {Is(1, {TYPE_BYTES}), Is(2, {TYPE_BYTES})},
                          },
                      },
              },
          },
          {
              "DETERMINISTIC_DECRYPT_BYTES",
              {
                  .rules =
                      {
                          {
                              .arity = 3,
                              .spelling = "bq_deterministic_decrypt_bytes($1, $2, $3)",
                              .conditions = {Is(1, {TYPE_BYTES})},
                          },
                      },
              },
          },
          {
              "DETERMINISTIC_DECRYPT_STRING",
              {
                  .rules =
                      {
                          {
                              .arity = 3,
                              .spelling = "bq_deterministic_decrypt_string($1, $2, encode($3))",
                              .conditions = {Is(1, {TYPE_BYTES})},
                          },
                      },
              },
          },
          {"KEYS.NEW_KEYSET", {.rules = {{.arity = 1, .spelling = "bq_keys_new_keyset($1)"}}}},
          {
              "KEYS.ROTATE_KEYSET",
              {.rules = {{.arity = 2, .spelling = "bq_keys_rotate_keyset($1, $2)"}}},
          },
          {
              "KEYS.KEYSET_LENGTH",
              {.rules = {{.arity = 1, .spelling = "bq_keys_keyset_length($1)"}}},
          },
          {
              "KEYS.KEYSET_TO_JSON",
              {.rules = {{.arity = 1, .spelling = "bq_keys_keyset_to_json($1)"}}},
          },
          {
              "KEYS.KEYSET_FROM_JSON",
              {.rules = {{.arity = 1, .spelling = "bq_keys_keyset_from_json($1)"}}},
          },
          {"HLL_COUNT.EXTRACT", {.rules = {{.arity = 1, .spelling = "bq_hll_count_extract($1)"}}}},
          // Without max_distance, the distance is not capped.
          {
              "EDIT_DISTANCE",
              {
                  .rules =
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
          },
          // Regular expressions. DuckDB raises no error for a replacement that refers to a missing
          // group, and returns '' or NULL elements where a capturing group takes no part in a
          // match.
          {"REGEXP_CONTAINS", {.rules = Strings("bq_regexp_contains", 2)}},
          {"REGEXP_REPLACE", {.rules = Strings("bq_regexp_replace", 3)}},
          {
              "REGEXP_EXTRACT",
              {
                  .rules =
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
          },
          {"REGEXP_EXTRACT_ALL", {.rules = Strings("bq_regexp_extract_all", 2)}},
          {
              "REGEXP_INSTR",
              {
                  .rules =
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
          },
          // JSON goes to GoogleSQL as its text; see src/backend_functions.cc.
          {
              "PARSE_JSON",
              {
                  .rules =
                      {
                          {
                              .arity = {1, 2},
                              .spelling = "json(bq_parse_json($1, $2))",
                              .conditions = {},
                              .defaults = {"'exact'"},
                          },
                      },
              },
          },
          {
              "BOOL",
              {
                  .rules =
                      {
                          {
                              .arity = 1,
                              .spelling = "bq_json_bool(CAST($1 AS VARCHAR))",
                              .conditions = {Is(1, {TYPE_JSON})},
                          },
                      },
              },
          },
          {
              "STRING",
              {
                  .rules =
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
          },
          {
              "INT64",
              {
                  .rules =
                      {
                          {
                              .arity = 1,
                              .spelling = "bq_json_int64(CAST($1 AS VARCHAR))",
                              .conditions = {Is(1, {TYPE_JSON})},
                          },
                      },
              },
          },
          {
              "FLOAT64",
              {
                  .rules =
                      {
                          {
                              .arity = {1, 2},
                              .spelling = "bq_json_float64(CAST($1 AS VARCHAR), $2)",
                              .conditions = {Is(1, {TYPE_JSON})},
                              .defaults = {"'round'"},
                          },
                      },
              },
          },
          {
              "DOUBLE",
              {
                  .rules =
                      {
                          {
                              .arity = {1, 2},
                              .spelling = "bq_json_float64(CAST($1 AS VARCHAR), $2)",
                              .conditions = {Is(1, {TYPE_JSON})},
                              .defaults = {"'round'"},
                          },
                      },
              },
          },
          {"JSON_TYPE", {.rules = {{.arity = 1, .spelling = "bq_json_type(CAST($1 AS VARCHAR))"}}}},
          {
              "$SUBSCRIPT",
              {
                  .rules =
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
          },
          {
              "JSON_FLATTEN",
              {
                  .rules =
                      {
                          {
                              .arity = 1,
                              .spelling = "CAST(bq_json_flatten(CAST($1 AS VARCHAR)) AS JSON[])",
                          },
                      },
              },
          },
          {"JSON_QUERY", {.rules = JsonExtract("bq_json_query", true, true)}},
          {"JSON_EXTRACT", {.rules = JsonExtract("bq_json_query", false, true)}},
          {"JSON_VALUE", {.rules = JsonExtract("bq_json_value", true, false)}},
          {"JSON_EXTRACT_SCALAR", {.rules = JsonExtract("bq_json_value", false, false)}},
          {"JSON_QUERY_ARRAY", {.rules = JsonExtract("bq_json_query_array", true, false)}},
          {"JSON_EXTRACT_ARRAY", {.rules = JsonExtract("bq_json_query_array", false, false)}},
          {"JSON_VALUE_ARRAY", {.rules = JsonExtract("bq_json_value_array", true, false)}},
          {
              "JSON_EXTRACT_STRING_ARRAY",
              {.rules = JsonExtract("bq_json_value_array", false, false)},
          },
          {
              "LAX_BOOL",
              {
                  .rules =
                      {
                          {
                              .arity = 1,
                              .spelling = "bq_lax_bool(CAST($1 AS VARCHAR))",
                              .conditions = {Is(1, {TYPE_JSON})},
                          },
                      },
              },
          },
          {
              "LAX_INT64",
              {
                  .rules =
                      {
                          {
                              .arity = 1,
                              .spelling = "bq_lax_int64(CAST($1 AS VARCHAR))",
                              .conditions = {Is(1, {TYPE_JSON})},
                          },
                      },
              },
          },
          {
              "LAX_FLOAT64",
              {
                  .rules =
                      {
                          {
                              .arity = 1,
                              .spelling = "bq_lax_float64(CAST($1 AS VARCHAR))",
                              .conditions = {Is(1, {TYPE_JSON})},
                          },
                      },
              },
          },
          {
              "LAX_DOUBLE",
              {
                  .rules =
                      {
                          {
                              .arity = 1,
                              .spelling = "bq_lax_float64(CAST($1 AS VARCHAR))",
                              .conditions = {Is(1, {TYPE_JSON})},
                          },
                      },
              },
          },
          {
              "LAX_STRING",
              {
                  .rules =
                      {
                          {
                              .arity = 1,
                              .spelling = "bq_lax_string(CAST($1 AS VARCHAR))",
                              .conditions = {Is(1, {TYPE_JSON})},
                          },
                      },
              },
          },
          {
              "JSON_KEYS",
              {
                  .rules =
                      {
                          {
                              .arity = 3,
                              .spelling = "CAST(json(bq_json_keys(CAST($1 AS VARCHAR), $2, $3)) AS "
                                          "VARCHAR[])",
                              .conditions = {Is(1, {TYPE_JSON})},
                          },
                      },
                  .uses = {TypeUse::kRejectsSafe},
              },
          },
          {
              "JSON_STRIP_NULLS",
              {
                  .rules =
                      {
                          {
                              .arity = 4,
                              .spelling =
                                  "json(bq_json_strip_nulls(CAST($1 AS VARCHAR), $2, $3, $4))",
                              .conditions = {Is(1, {TYPE_JSON})},
                          },
                      },
              },
          },
  };
  return kRules;
}

}  // namespace bigquery_emulator_duckdb::translator
