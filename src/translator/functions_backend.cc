#include "src/translator/functions_backend.h"

#include <array>
#include <span>
#include <string_view>
#include <tuple>

#include "src/function_catalog/rules.h"
#include "src/function_catalog/static_definitions.h"

namespace bigquery_emulator_duckdb::translator {

using function_catalog::Define;
using function_catalog::DefinitionViews;

using function_catalog::Arity;
using function_catalog::StaticCondition;
using function_catalog::StaticRule;
using function_catalog::TypeUse;

namespace {

using enum googlesql::TypeKind;

template <googlesql::TypeKind kType>
struct ArgumentType {
  static constexpr std::array kTypes = {kType};
  static constexpr StaticCondition kFirst[] = {{.argument = 1, .types = kTypes}};
};

constexpr std::string_view kMaxLength[] = {"9223372036854775807"};
constexpr std::string_view kUtcDefault[] = {"'UTC'"};
constexpr std::string_view kFirstOccurrence[] = {"1", "1"};
constexpr std::string_view kFirstOccurrenceStart[] = {"1", "1", "0"};
constexpr std::string_view kCommaDefault[] = {"','"};
constexpr std::string_view kNfcDefault[] = {"'NFC'"};
constexpr std::string_view kExactDefault[] = {"'exact'"};
constexpr std::string_view kRoundDefault[] = {"'round'"};
constexpr std::string_view kSpaceDefault[] = {"' '"};
constexpr std::string_view kBytesSpaceDefault[] = {"encode(' ')"};
constexpr std::string_view kJsonPathDefault[] = {"'$'"};
constexpr std::string_view kTrueDefault[] = {"true"};

// These helpers only assemble views of static data; they do not generate SQL or own strings.
constexpr std::array<StaticRule, 2> StringBytes(Arity arity, std::string_view text,
                                                std::string_view bytes,
                                                std::span<const std::string_view> defaults = {}) {
  return {
      {
          {
              .arity = arity,
              .spelling = text,
              .conditions = ArgumentType<TYPE_STRING>::kFirst,
              .defaults = defaults,
          },
          {
              .arity = arity,
              .spelling = bytes,
              .conditions = ArgumentType<TYPE_BYTES>::kFirst,
              .defaults = defaults,
          },
      },
  };
}

constexpr StaticRule Json(Arity arity, std::string_view spelling,
                          std::span<const std::string_view> defaults = {}) {
  return {
      .arity = arity,
      .spelling = spelling,
      .conditions = ArgumentType<TYPE_JSON>::kFirst,
      .defaults = defaults,
  };
}

// JSON_QUERY and the other extractions of STRING or JSON.
constexpr std::array<StaticRule, 2> JsonExtract(std::string_view json, std::string_view text) {
  return {
      {
          Json({1, 2}, json, kJsonPathDefault),
          {
              .arity = {1, 2},
              .spelling = text,
              .conditions = ArgumentType<TYPE_STRING>::kFirst,
              .defaults = kJsonPathDefault,
          },
      },
  };
}

// Decimal overloads retain precision rather than going through FLOAT64.
constexpr std::array<StaticRule, 3> Numbers(Arity arity, std::string_view bignumeric,
                                            std::string_view floating, std::string_view numeric) {
  return {
      {
          {
              .arity = arity,
              .spelling = bignumeric,
              .conditions = ArgumentType<TYPE_BIGNUMERIC>::kFirst,
          },
          {.arity = arity, .spelling = floating, .conditions = ArgumentType<TYPE_DOUBLE>::kFirst},
          {.arity = arity, .spelling = numeric, .conditions = ArgumentType<TYPE_NUMERIC>::kFirst},
      },
  };
}

// Without explicit characters, trim Unicode whitespace rather than DuckDB's spaces.
constexpr std::array<StaticRule, 3> Trim(std::string_view whitespace, std::string_view characters,
                                         std::string_view bytes) {
  return {
      {
          {.arity = 1, .spelling = whitespace, .conditions = ArgumentType<TYPE_STRING>::kFirst},
          {.arity = 2, .spelling = characters, .conditions = ArgumentType<TYPE_STRING>::kFirst},
          {.arity = 2, .spelling = bytes, .conditions = ArgumentType<TYPE_BYTES>::kFirst},
      },
  };
}

constexpr std::array<StaticRule, 2> Pad(std::string_view text, std::string_view bytes) {
  auto rules = StringBytes({2, 3}, text, bytes, kSpaceDefault);
  rules.at(1).defaults = kBytesSpaceDefault;
  return rules;
}

}  // namespace

// Literal spellings and metadata backing arrays have static storage.
std::span<const function_catalog::StaticNamedDefinition<StaticRule>> StaticBackendRules() {
  static constexpr std::string_view kIntervalParts[] = {
      "year", "quarter", "month",  "week",        "day",
      "hour", "minute",  "second", "millisecond", "microsecond",
  };
  static constexpr StaticCondition kIntervalPart[] = {
      {.argument = 2, .date_parts = kIntervalParts},
  };
  // STRING and BYTES use GoogleSQL's position, length and NULL semantics.
  static constexpr auto kSubstr =
      StringBytes({2, 3}, "bq_substr($1, $2, $3)", "bq_substr_bytes($1, $2, $3)", kMaxLength);
  // FORMAT_DATE, FORMAT_DATETIME and FORMAT_TIMESTAMP each also take the other two types.
  static constexpr StaticCondition kDateValue[] = {
      {.argument = 2, .types = ArgumentType<TYPE_DATE>::kTypes},
  };
  static constexpr StaticCondition kDateTimeValue[] = {
      {.argument = 2, .types = ArgumentType<TYPE_DATETIME>::kTypes},
  };
  static constexpr StaticCondition kTimestampValue[] = {
      {.argument = 2, .types = ArgumentType<TYPE_TIMESTAMP>::kTypes},
  };
  static constexpr StaticRule kFormatDateTime[] = {
      {.arity = 2, .spelling = "bq_format_date($1, $2)", .conditions = kDateValue},
      {.arity = 2, .spelling = "bq_format_datetime($1, $2)", .conditions = kDateTimeValue},
      {
          .arity = {2, 3},
          .spelling = "bq_format_timestamp($1, $2, $3)",
          .conditions = kTimestampValue,
          .defaults = kUtcDefault,
      },
  };
  // AEAD uses Tink AES-GCM, deterministic AEAD uses AES-SIV; STRING is its UTF-8 bytes.
  // Wrapped keysets and KEYS.KEYSET_CHAIN need Cloud KMS.
  static constexpr StaticCondition kBytesString[] = {
      {.argument = 1, .types = ArgumentType<TYPE_BYTES>::kTypes},
      {.argument = 2, .types = ArgumentType<TYPE_STRING>::kTypes},
  };
  static constexpr StaticCondition kBytesBytes[] = {
      {.argument = 1, .types = ArgumentType<TYPE_BYTES>::kTypes},
      {.argument = 2, .types = ArgumentType<TYPE_BYTES>::kTypes},
  };
  static constexpr StaticRule kFloat64[] = {
      Json({1, 2}, "bq_json_float64(CAST($1 AS VARCHAR), $2)", kRoundDefault),
  };
  static constexpr StaticCondition kJsonField[] = {
      {.argument = 1, .types = ArgumentType<TYPE_JSON>::kTypes},
      {.argument = 2, .types = ArgumentType<TYPE_STRING>::kTypes},
  };
  static constexpr StaticCondition kJsonElement[] = {
      {.argument = 1, .types = ArgumentType<TYPE_JSON>::kTypes},
      {.argument = 2, .types = ArgumentType<TYPE_INT64>::kTypes},
  };
  static constexpr StaticRule kLaxFloat64[] = {
      Json(1, "bq_lax_float64(CAST($1 AS VARCHAR))"),
  };
  static constexpr auto kPow =
      Numbers(2, "CAST(bq_bignumeric_pow(CAST($1 AS VARCHAR), CAST($2 AS VARCHAR)) AS BIGNUM)",
              "bq_pow($1, $2)", "bq_pow_numeric($1, $2)");
  static constexpr auto kLn = Numbers(1, "CAST(bq_bignumeric_ln(CAST($1 AS VARCHAR)) AS BIGNUM)",
                                      "bq_ln($1)", "bq_ln_numeric($1)");
  static constexpr auto kLogBinary =
      Numbers(2, "CAST(bq_bignumeric_log(CAST($1 AS VARCHAR), CAST($2 AS VARCHAR)) AS BIGNUM)",
              "bq_log($1, $2)", "bq_log_numeric($1, $2)");
  static constexpr auto kRulesBacking = std::tuple{
      // DuckDB returns infinities where BigQuery raises errors, such as EXP(1000).

      Define("SQRT",
             Numbers(1, "CAST(bq_bignumeric_sqrt(CAST($1 AS VARCHAR)) AS BIGNUM)", "bq_sqrt($1)",
                     "bq_sqrt_numeric($1)"),
             {
                 .uses = {TypeUse::kBigNumeric},
             }),

      Define("CBRT",
             Numbers(1, "CAST(bq_bignumeric_cbrt(CAST($1 AS VARCHAR)) AS BIGNUM)", "bq_cbrt($1)",
                     "bq_cbrt_numeric($1)"),
             {
                 .uses = {TypeUse::kBigNumeric},
             }),
      Define("POW", kPow, {.uses = {TypeUse::kBigNumeric}}),
      Define("POWER", kPow, {.uses = {TypeUse::kBigNumeric}}),

      Define("EXP",
             Numbers(1, "CAST(bq_bignumeric_exp(CAST($1 AS VARCHAR)) AS BIGNUM)", "bq_exp($1)",
                     "bq_exp_numeric($1)"),
             {
                 .uses = {TypeUse::kBigNumeric},
             }),
      Define("LN", kLn, {.uses = {TypeUse::kBigNumeric}}),

      Define("LOG10",
             Numbers(1, "CAST(bq_bignumeric_log10(CAST($1 AS VARCHAR)) AS BIGNUM)", "bq_log10($1)",
                     "bq_log10_numeric($1)"),
             {
                 .uses = {TypeUse::kBigNumeric},
             }),

      Define("LOG",
             {
                 kLn.at(0),
                 kLn.at(1),
                 kLn.at(2),
                 kLogBinary.at(0),
                 kLogBinary.at(1),
                 kLogBinary.at(2),
             },
             {
                 .uses = {TypeUse::kBigNumeric},
             }),

      // GoogleSQL splits the RANGE into bounds, rejects unsupported steps and
      // unbounded ends, and keeps the last partial subrange by default.
      Define("GENERATE_RANGE_ARRAY",
             {
                 {
                     .arity = {2, 3},
                     .spelling = "list_transform([bq_generate_range_array(struct_extract($1, "
                                 "'$start'), "
                                 "struct_extract($1, '$end'), $2, $3)], lambda _b: "
                                 "list_transform(range(1, len(_b)), lambda _i: "
                                 "struct_pack(\"$start\" := _b[_i], \"$end\" := _b[_i + 1])))[1]",
                     .defaults = kTrueDefault,
                 },
             },
             {
                 .uses = {TypeUse::kBuildsOrReadsInterval, TypeUse::kBuildsOrReadsRange},
             }),
      // The final boolean selects standard JSONPath syntax.

      Define("JSON_QUERY", JsonExtract("json(bq_json_query_json(CAST($1 AS VARCHAR), $2, true))",
                                       "bq_json_query($1, $2, true)")),

      Define("JSON_EXTRACT", JsonExtract("json(bq_json_query_json(CAST($1 AS VARCHAR), $2, false))",
                                         "bq_json_query($1, $2, false)")),

      Define("JSON_VALUE", JsonExtract("bq_json_value_json(CAST($1 AS VARCHAR), $2, true)",
                                       "bq_json_value($1, $2, true)")),

      Define("JSON_EXTRACT_SCALAR",
             JsonExtract("bq_json_value_json(CAST($1 AS VARCHAR), $2, false)",
                         "bq_json_value($1, $2, false)")),

      Define("JSON_QUERY_ARRAY",
             JsonExtract("bq_json_query_array_json(CAST($1 AS VARCHAR), $2, true)",
                         "bq_json_query_array($1, $2, true)")),

      Define("JSON_EXTRACT_ARRAY",
             JsonExtract("bq_json_query_array_json(CAST($1 AS VARCHAR), $2, false)",
                         "bq_json_query_array($1, $2, false)")),

      Define("JSON_VALUE_ARRAY",
             JsonExtract("bq_json_value_array_json(CAST($1 AS VARCHAR), $2, true)",
                         "bq_json_value_array($1, $2, true)")),

      Define("JSON_EXTRACT_STRING_ARRAY",
             JsonExtract("bq_json_value_array_json(CAST($1 AS VARCHAR), $2, false)",
                         "bq_json_value_array($1, $2, false)")),

      Define("$INTERVAL",
             {
                 {
                     .arity = 2,
                     .spelling = "bq_interval($1, upper(#2))",
                     .conditions = kIntervalPart,
                 },
             },
             {
                 .uses = {TypeUse::kBuildsOrReadsInterval},
             }),

      Define("MAKE_INTERVAL",
             {
                 {.arity = 6, .spelling = "bq_make_interval($1, $2, $3, $4, $5, $6)"},
             },
             {
                 .uses = {TypeUse::kBuildsOrReadsInterval},
             }),

      Define("JUSTIFY_HOURS", {{.arity = 1, .spelling = "bq_justify_hours($1)"}},
             {
                 .uses = {TypeUse::kBuildsOrReadsInterval},
             }),

      Define("JUSTIFY_DAYS", {{.arity = 1, .spelling = "bq_justify_days($1)"}},
             {
                 .uses = {TypeUse::kBuildsOrReadsInterval},
             }),

      Define("JUSTIFY_INTERVAL",
             {
                 {.arity = 1, .spelling = "bq_justify_interval($1)"},
             },
             {
                 .uses = {TypeUse::kBuildsOrReadsInterval},
             }),
      Define("PARSE_NUMERIC", {{.arity = 1, .spelling = "bq_parse_numeric($1)"}}),

      Define("PARSE_BIGNUMERIC",
             {
                 {.arity = 1, .spelling = "CAST(bq_parse_bignumeric($1) AS BIGNUM)"},
             },
             {
                 .uses = {TypeUse::kBigNumeric},
             }),

      Define("COSINE_DISTANCE",
             {
                 {.arity = 2, .spelling = "bq_cosine_distance($1, $2)"},
             }),

      Define("EUCLIDEAN_DISTANCE",
             {
                 {.arity = 2, .spelling = "bq_euclidean_distance($1, $2)"},
             }),
      Define("CHR", {{.arity = 1, .spelling = "bq_chr($1)"}}),
      // DuckDB's BLOB has almost no functions; its Unicode and error semantics differ too.
      Define("REPEAT", StringBytes(2, "bq_repeat($1, $2)", "bq_repeat_bytes($1, $2)")),
      Define("LOWER", StringBytes(1, "bq_lower($1)", "bq_lower_bytes($1)")),
      Define("UPPER", StringBytes(1, "bq_upper($1)", "bq_upper_bytes($1)")),
      Define("REVERSE", StringBytes(1, "bq_reverse($1)", "bq_reverse_bytes($1)")),
      Define("REPLACE", StringBytes(3, "bq_replace($1, $2, $3)", "bq_replace_bytes($1, $2, $3)")),
      Define("TRANSLATE",
             StringBytes(3, "bq_translate($1, $2, $3)", "bq_translate_bytes($1, $2, $3)")),
      Define("ASCII", StringBytes(1, "bq_ascii($1)", "bq_ascii_bytes($1)")),

      Define("UNICODE",
             {
                 {
                     .arity = 1,
                     .spelling = "bq_unicode($1)",
                     .conditions = ArgumentType<TYPE_STRING>::kFirst,
                 },
             }),
      Define("TO_CODE_POINTS",
             StringBytes(1, "bq_to_code_points($1)", "bq_to_code_points_bytes($1)")),
      // CODE_POINTS_TO_* is NULL when an element is NULL.

      Define("CODE_POINTS_TO_STRING",
             {
                 {
                     .arity = 1,
                     .spelling = "CASE WHEN list_count($1) <> len($1) THEN NULL ELSE "
                                 "bq_code_points_to_string($1) END",
                 },
             }),

      Define("CODE_POINTS_TO_BYTES",
             {
                 {
                     .arity = 1,
                     .spelling = "CASE WHEN list_count($1) <> len($1) THEN NULL ELSE "
                                 "bq_code_points_to_bytes($1) END",
                 },
             }),
      Define("LEFT", StringBytes(2, "bq_left($1, $2)", "bq_left_bytes($1, $2)")),
      Define("RIGHT", StringBytes(2, "bq_right($1, $2)", "bq_right_bytes($1, $2)")),
      Define("LPAD", Pad("bq_lpad($1, $2, $3)", "bq_lpad_bytes($1, $2, $3)")),
      Define("RPAD", Pad("bq_rpad($1, $2, $3)", "bq_rpad_bytes($1, $2, $3)")),
      Define("TRIM", Trim("bq_trim($1)", "bq_trim_chars($1, $2)", "bq_trim_bytes($1, $2)")),
      Define("LTRIM", Trim("bq_ltrim($1)", "bq_ltrim_chars($1, $2)", "bq_ltrim_bytes($1, $2)")),
      Define("RTRIM", Trim("bq_rtrim($1)", "bq_rtrim_chars($1, $2)", "bq_rtrim_bytes($1, $2)")),
      // DuckDB's regexp replacement and nonparticipating captures have different semantics.

      Define("REGEXP_CONTAINS",
             StringBytes(2, "bq_regexp_contains($1, $2)", "bq_regexp_contains_bytes($1, $2)")),

      Define("REGEXP_REPLACE", StringBytes(3, "bq_regexp_replace($1, $2, $3)",
                                           "bq_regexp_replace_bytes($1, $2, $3)")),

      Define("REGEXP_EXTRACT_ALL", StringBytes(2, "bq_regexp_extract_all($1, $2)",
                                               "bq_regexp_extract_all_bytes($1, $2)")),
      Define("SUBSTR", kSubstr),
      Define("SUBSTRING", kSubstr),

      Define("INSTR", StringBytes({2, 4}, "bq_instr($1, $2, $3, $4)",
                                  "bq_instr_bytes($1, $2, $3, $4)", kFirstOccurrence)),
      Define("STRPOS", StringBytes(2, "strpos($1, $2)", "bq_instr_bytes($1, $2, 1, 1)")),
      Define("STARTS_WITH", StringBytes(2, "starts_with($1, $2)", "bq_starts_with_bytes($1, $2)")),
      Define("ENDS_WITH", StringBytes(2, "ends_with($1, $2)", "bq_ends_with_bytes($1, $2)")),

      // CASE avoids DuckDB v2's IF macro binder.
      Define("SPLIT",
             {
                 {
                     .arity = {1, 2},
                     .spelling = "CASE WHEN $2 IS NULL THEN NULL ELSE split($1, $2) END",
                     .conditions = ArgumentType<TYPE_STRING>::kFirst,
                     .defaults = kCommaDefault,
                 },
                 {
                     .arity = 2,
                     .spelling = "bq_split_bytes($1, $2)",
                     .conditions = ArgumentType<TYPE_BYTES>::kFirst,
                 },
             }),
      // GoogleSQL stops before overflow and rejects zero steps and arrays beyond BigQuery's limit.

      Define("GENERATE_DATE_ARRAY",
             {
                 {.arity = 2, .spelling = "bq_generate_date_array($1, $2, 1, 'day')"},
                 {.arity = 4, .spelling = "bq_generate_date_array($1, $2, $3, #4)"},
             }),
      Define("FORMAT_DATE", kFormatDateTime),
      Define("FORMAT_DATETIME", kFormatDateTime),
      Define("FORMAT_TIMESTAMP", kFormatDateTime),
      Define("FORMAT_TIME", {{.arity = 2, .spelling = "bq_format_time($1, $2)"}}),
      Define("MONTHS_BETWEEN",
             {
                 {
                     .arity = 2,
                     .spelling = "bq_months_between_date($1, $2)",
                     .conditions = ArgumentType<TYPE_DATE>::kFirst,
                 },
                 {
                     .arity = 2,
                     .spelling = "bq_months_between_datetime($1, $2)",
                     .conditions = ArgumentType<TYPE_DATETIME>::kFirst,
                 },
             }),
      Define("PARSE_DATE", {{.arity = 2, .spelling = "bq_parse_date($1, $2)"}}),

      Define("PARSE_DATETIME",
             {
                 {.arity = 2, .spelling = "bq_parse_datetime($1, $2)"},
             }),
      Define("PARSE_TIME", {{.arity = 2, .spelling = "bq_parse_time($1, $2)"}}),

      Define("PARSE_TIMESTAMP",
             {
                 {
                     .arity = {2, 3},
                     .spelling = "bq_parse_timestamp($1, $2, $3)",
                     .defaults = kUtcDefault,
                 },
             }),
      // The normalization mode is passed by its name.

      Define("NORMALIZE",
             {
                 {
                     .arity = {1, 2},
                     .spelling = "bq_normalize($1, $2)",
                     .defaults = kNfcDefault,
                 },
             }),

      Define("NORMALIZE_AND_CASEFOLD",
             {
                 {
                     .arity = {1, 2},
                     .spelling = "bq_normalize_and_casefold($1, $2)",
                     .defaults = kNfcDefault,
                 },
             }),
      // encode() takes a STRING's UTF-8 bytes.
      Define("SHA512", StringBytes(1, "bq_sha512(encode($1))", "bq_sha512($1)")),
      Define("FARM_FINGERPRINT",
             StringBytes(1, "bq_farm_fingerprint(encode($1))", "bq_farm_fingerprint($1)")),
      // GoogleSQL leaves base32 out of its open source; string.cc implements RFC 4648's.
      Define("TO_BASE32", {{.arity = 1, .spelling = "bq_to_base32($1)"}}),
      Define("FROM_BASE32", {{.arity = 1, .spelling = "bq_from_base32($1)"}}),

      Define("INITCAP",
             {
                 {.arity = 1, .spelling = "bq_initcap($1)"},
                 {.arity = 2, .spelling = "bq_initcap_delimiters($1, $2)"},
             }),
      Define("SOUNDEX", {{.arity = 1, .spelling = "bq_soundex($1)"}}),

      Define("SAFE_CONVERT_BYTES_TO_STRING",
             {
                 {.arity = 1, .spelling = "bq_safe_convert_bytes_to_string($1)"},
             }),
      // HOST, REG_DOMAIN and PUBLIC_SUFFIX follow GoogleSQL's copy of the public suffix list.

      Define("NET.IPV4_FROM_INT64",
             {
                 {.arity = 1, .spelling = "bq_net_ipv4_from_int64($1)"},
             }),

      Define("NET.IPV4_TO_INT64",
             {
                 {.arity = 1, .spelling = "bq_net_ipv4_to_int64($1)"},
             }),

      Define("NET.IP_FROM_STRING",
             {
                 {.arity = 1, .spelling = "bq_net_ip_from_string($1)"},
             }),

      Define("NET.SAFE_IP_FROM_STRING",
             {
                 {.arity = 1, .spelling = "bq_net_safe_ip_from_string($1)"},
             }),
      Define("NET.IP_TO_STRING", {{.arity = 1, .spelling = "bq_net_ip_to_string($1)"}}),

      Define("NET.IP_NET_MASK",
             {
                 {.arity = 2, .spelling = "bq_net_ip_net_mask($1, $2)"},
             }),
      Define("NET.IP_TRUNC", {{.arity = 2, .spelling = "bq_net_ip_trunc($1, $2)"}}),
      Define("NET.HOST", {{.arity = 1, .spelling = "bq_net_host($1)"}}),
      Define("NET.REG_DOMAIN", {{.arity = 1, .spelling = "bq_net_reg_domain($1)"}}),

      Define("NET.PUBLIC_SUFFIX",
             {
                 {.arity = 1, .spelling = "bq_net_public_suffix($1)"},
             }),

      Define("AEAD.ENCRYPT",
             {
                 {
                     .arity = 3,
                     .spelling = "bq_aead_encrypt($1, encode($2), encode($3))",
                     .conditions = kBytesString,
                 },
                 {
                     .arity = 3,
                     .spelling = "bq_aead_encrypt($1, $2, $3)",
                     .conditions = kBytesBytes,
                 },
             }),

      Define("AEAD.DECRYPT_BYTES",
             {
                 {
                     .arity = 3,
                     .spelling = "bq_aead_decrypt_bytes($1, $2, $3)",
                     .conditions = ArgumentType<TYPE_BYTES>::kFirst,
                 },
             }),

      Define("AEAD.DECRYPT_STRING",
             {
                 {
                     .arity = 3,
                     .spelling = "bq_aead_decrypt_string($1, $2, encode($3))",
                     .conditions = ArgumentType<TYPE_BYTES>::kFirst,
                 },
             }),

      Define("DETERMINISTIC_ENCRYPT",
             {
                 {
                     .arity = 3,
                     .spelling = "bq_deterministic_encrypt($1, encode($2), encode($3))",
                     .conditions = kBytesString,
                 },
                 {
                     .arity = 3,
                     .spelling = "bq_deterministic_encrypt($1, $2, $3)",
                     .conditions = kBytesBytes,
                 },
             }),

      Define("DETERMINISTIC_DECRYPT_BYTES",
             {
                 {
                     .arity = 3,
                     .spelling = "bq_deterministic_decrypt_bytes($1, $2, $3)",
                     .conditions = ArgumentType<TYPE_BYTES>::kFirst,
                 },
             }),

      Define("DETERMINISTIC_DECRYPT_STRING",
             {
                 {
                     .arity = 3,
                     .spelling = "bq_deterministic_decrypt_string($1, $2, encode($3))",
                     .conditions = ArgumentType<TYPE_BYTES>::kFirst,
                 },
             }),
      Define("KEYS.NEW_KEYSET", {{.arity = 1, .spelling = "bq_keys_new_keyset($1)"}}),

      Define("KEYS.ROTATE_KEYSET",
             {
                 {.arity = 2, .spelling = "bq_keys_rotate_keyset($1, $2)"},
             }),

      Define("KEYS.KEYSET_LENGTH",
             {
                 {.arity = 1, .spelling = "bq_keys_keyset_length($1)"},
             }),

      Define("KEYS.KEYSET_TO_JSON",
             {
                 {.arity = 1, .spelling = "bq_keys_keyset_to_json($1)"},
             }),

      Define("KEYS.KEYSET_FROM_JSON",
             {
                 {.arity = 1, .spelling = "bq_keys_keyset_from_json($1)"},
             }),

      Define("HLL_COUNT.EXTRACT",
             {
                 {.arity = 1, .spelling = "bq_hll_count_extract($1)"},
             }),
      // Without max_distance the distance is not capped.

      Define("EDIT_DISTANCE", StringBytes({2, 3}, "bq_edit_distance($1, $2, $3)",
                                          "bq_edit_distance_bytes($1, $2, $3)", kMaxLength)),

      Define("REGEXP_EXTRACT",
             StringBytes({2, 4}, "bq_regexp_extract($1, $2, $3, $4)",
                         "bq_regexp_extract_bytes($1, $2, $3, $4)", kFirstOccurrence)),

      Define("REGEXP_INSTR",
             StringBytes({2, 5}, "bq_regexp_instr($1, $2, $3, $4, $5)",
                         "bq_regexp_instr_bytes($1, $2, $3, $4, $5)", kFirstOccurrenceStart)),
      // JSON goes to GoogleSQL as text.

      Define("PARSE_JSON",
             {
                 {
                     .arity = {1, 2},
                     .spelling = "json(bq_parse_json($1, $2))",
                     .defaults = kExactDefault,
                 },
             }),
      Define("BOOL", {Json(1, "bq_json_bool(CAST($1 AS VARCHAR))")}),

      Define("STRING",
             {
                 Json(1, "bq_json_string(CAST($1 AS VARCHAR))"),
                 {
                     .arity = {1, 2},
                     .spelling = "bq_timestamp_string($1, $2)",
                     .conditions = ArgumentType<TYPE_TIMESTAMP>::kFirst,
                     .defaults = kUtcDefault,
                 },
             }),
      Define("INT64", {Json(1, "bq_json_int64(CAST($1 AS VARCHAR))")}),
      Define("FLOAT64", kFloat64),
      Define("DOUBLE", kFloat64),

      Define("JSON_TYPE",
             {
                 {.arity = 1, .spelling = "bq_json_type(CAST($1 AS VARCHAR))"},
             }),

      Define("$SUBSCRIPT",
             {
                 {
                     .arity = 2,
                     .spelling = "json(bq_json_field(CAST($1 AS VARCHAR), $2))",
                     .conditions = kJsonField,
                 },
                 {
                     .arity = 2,
                     .spelling = "json(bq_json_element(CAST($1 AS VARCHAR), $2))",
                     .conditions = kJsonElement,
                 },
             }),

      Define("JSON_FLATTEN",
             {
                 {
                     .arity = 1,
                     .spelling = "CAST(bq_json_flatten(CAST($1 AS VARCHAR)) AS JSON[])",
                 },
             }),
      Define("LAX_BOOL", {Json(1, "bq_lax_bool(CAST($1 AS VARCHAR))")}),
      Define("LAX_INT64", {Json(1, "bq_lax_int64(CAST($1 AS VARCHAR))")}),
      Define("LAX_FLOAT64", kLaxFloat64),
      Define("LAX_DOUBLE", kLaxFloat64),
      Define("LAX_STRING", {Json(1, "bq_lax_string(CAST($1 AS VARCHAR))")}),

      Define("JSON_KEYS",
             {
                 Json(3, "CAST(json(bq_json_keys(CAST($1 AS VARCHAR), $2, $3)) AS VARCHAR[])"),
             },
             {
                 .uses = {TypeUse::kRejectsSafe},
             }),

      Define("JSON_STRIP_NULLS",
             {
                 Json(4, "json(bq_json_strip_nulls(CAST($1 AS VARCHAR), $2, $3, $4))"),
             }),
  };
  static constexpr auto kRules = DefinitionViews(kRulesBacking);
  return kRules;
}

}  // namespace bigquery_emulator_duckdb::translator
