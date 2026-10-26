#include "src/translator/functions_backend.h"

#include <array>
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

using function_catalog::Arity;
using function_catalog::BigNumericOperator;
using function_catalog::Concat;
using function_catalog::Is;
using function_catalog::Rule;
using function_catalog::Same;
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

// GoogleSQL splits the RANGE into bounds, rejects unsupported steps and unbounded ends,
// and keeps the last partial subrange by default.
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
          .defaults = {"true"},
      },
  };
}

std::vector<Rule> Float64(std::string_view function, std::size_t arity = 1) {
  return Same(function, arity, {Is(1, {TYPE_DOUBLE})});
}

// JSON_QUERY and the other extractions of STRING or JSON. `standard` selects JSONPath syntax.
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

// The backend uses `function` for STRING and `function`_bytes for BYTES.
std::vector<Rule> Strings(std::string_view function, Arity arity) {
  return Concat({
      Same(function, arity, {Is(1, {TYPE_STRING})}),
      Same(std::string(function) + "_bytes", arity, {Is(1, {TYPE_BYTES})}),
  });
}

// Decimal overloads retain precision rather than going through FLOAT64.
std::vector<Rule> Numbers(std::string_view name, std::size_t arity = 1) {
  const std::string function = "bq_" + std::string(name);
  return Concat({
      {BigNumericOperator("bq_bignumeric_" + std::string(name), arity)},
      Float64(function, arity),
      Same(function + "_numeric", arity, {Is(1, {TYPE_NUMERIC})}),
  });
}

// CODE_POINTS_TO_* is NULL when an element is NULL.
std::vector<Rule> CodePointsTo(const std::string& function) {
  return {
      {
          .arity = 1,
          .spelling = "CASE WHEN list_count($1) <> len($1) THEN NULL ELSE " + function + "($1) END",
      },
  };
}

// Without explicit characters, trim Unicode whitespace rather than DuckDB's spaces.
std::vector<Rule> Trim(const std::string& function) {
  return {
      {.arity = 1, .spelling = function + "($1)", .conditions = {Is(1, {TYPE_STRING})}},
      {.arity = 2, .spelling = function + "_chars($1, $2)", .conditions = {Is(1, {TYPE_STRING})}},
      {.arity = 2, .spelling = function + "_bytes($1, $2)", .conditions = {Is(1, {TYPE_BYTES})}},
  };
}

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

// Literal spellings and all their backing arrays have static storage.
std::span<const function_catalog::StaticNamedDefinition<StaticRule>> StaticBackendRules() {
  static constexpr std::string_view kIntervalParts[] = {
      "year", "quarter", "month",  "week",        "day",
      "hour", "minute",  "second", "millisecond", "microsecond",
  };
  static constexpr StaticCondition kIntervalPart[] = {
      {.argument = 2, .date_parts = kIntervalParts},
  };
  static constexpr StaticRule kInterval[] = {
      {.arity = 2, .spelling = "bq_interval($1, upper(#2))", .conditions = kIntervalPart},
  };
  static constexpr StaticRule kMakeInterval[] = {
      {.arity = 6, .spelling = "bq_make_interval($1, $2, $3, $4, $5, $6)"},
  };
  static constexpr StaticRule kJustifyHours[] = {{.arity = 1, .spelling = "bq_justify_hours($1)"}};
  static constexpr StaticRule kJustifyDays[] = {{.arity = 1, .spelling = "bq_justify_days($1)"}};
  static constexpr StaticRule kJustifyInterval[] = {
      {.arity = 1, .spelling = "bq_justify_interval($1)"},
  };
  static constexpr StaticRule kParseNumeric[] = {{.arity = 1, .spelling = "bq_parse_numeric($1)"}};
  static constexpr StaticRule kParseBigNumeric[] = {
      {.arity = 1, .spelling = "CAST(bq_parse_bignumeric($1) AS BIGNUM)"},
  };
  static constexpr StaticRule kCosineDistance[] = {
      {.arity = 2, .spelling = "bq_cosine_distance($1, $2)"},
  };
  static constexpr StaticRule kEuclideanDistance[] = {
      {.arity = 2, .spelling = "bq_euclidean_distance($1, $2)"},
  };
  static constexpr StaticRule kChr[] = {{.arity = 1, .spelling = "bq_chr($1)"}};
  // GoogleSQL stops before overflow and rejects zero steps and arrays beyond BigQuery's limit.
  static constexpr StaticRule kGenerateDateArray[] = {
      {.arity = 2, .spelling = "bq_generate_date_array($1, $2, 1, 'day')"},
      {.arity = 4, .spelling = "bq_generate_date_array($1, $2, $3, #4)"},
  };
  // STRING and BYTES use GoogleSQL's position, length and NULL semantics.
  static constexpr auto kSubstr =
      StringBytes({2, 3}, "bq_substr($1, $2, $3)", "bq_substr_bytes($1, $2, $3)", kMaxLength);
  static constexpr auto kInstr = StringBytes({2, 4}, "bq_instr($1, $2, $3, $4)",
                                             "bq_instr_bytes($1, $2, $3, $4)", kFirstOccurrence);
  static constexpr auto kStrpos = StringBytes(2, "strpos($1, $2)", "bq_instr_bytes($1, $2, 1, 1)");
  static constexpr auto kStartsWith =
      StringBytes(2, "starts_with($1, $2)", "bq_starts_with_bytes($1, $2)");
  static constexpr auto kEndsWith =
      StringBytes(2, "ends_with($1, $2)", "bq_ends_with_bytes($1, $2)");
  static constexpr StaticRule kSplit[] = {
      {
          .arity = {1, 2},
          .spelling = "split($1, $2)",
          .conditions = ArgumentType<TYPE_STRING>::kFirst,
          .defaults = kCommaDefault,
      },
      {
          .arity = 2,
          .spelling = "bq_split_bytes($1, $2)",
          .conditions = ArgumentType<TYPE_BYTES>::kFirst,
      },
  };
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
  static constexpr StaticRule kFormatTime[] = {{.arity = 2, .spelling = "bq_format_time($1, $2)"}};
  static constexpr StaticRule kParseDate[] = {{.arity = 2, .spelling = "bq_parse_date($1, $2)"}};
  static constexpr StaticRule kParseDateTime[] = {
      {.arity = 2, .spelling = "bq_parse_datetime($1, $2)"},
  };
  static constexpr StaticRule kParseTime[] = {{.arity = 2, .spelling = "bq_parse_time($1, $2)"}};
  static constexpr StaticRule kParseTimestamp[] = {
      {.arity = {2, 3}, .spelling = "bq_parse_timestamp($1, $2, $3)", .defaults = kUtcDefault},
  };
  // The normalization mode is passed by its name.
  static constexpr StaticRule kNormalize[] = {
      {.arity = {1, 2}, .spelling = "bq_normalize($1, $2)", .defaults = kNfcDefault},
  };
  static constexpr StaticRule kNormalizeCasefold[] = {
      {.arity = {1, 2}, .spelling = "bq_normalize_and_casefold($1, $2)", .defaults = kNfcDefault},
  };
  // encode() takes a STRING's UTF-8 bytes.
  static constexpr auto kSha512 = StringBytes(1, "bq_sha512(encode($1))", "bq_sha512($1)");
  static constexpr auto kFarmFingerprint =
      StringBytes(1, "bq_farm_fingerprint(encode($1))", "bq_farm_fingerprint($1)");
  // GoogleSQL leaves base32 out of its open source; string.cc implements RFC 4648's.
  static constexpr StaticRule kToBase32[] = {{.arity = 1, .spelling = "bq_to_base32($1)"}};
  static constexpr StaticRule kFromBase32[] = {{.arity = 1, .spelling = "bq_from_base32($1)"}};
  static constexpr StaticRule kInitcap[] = {
      {.arity = 1, .spelling = "bq_initcap($1)"},
      {.arity = 2, .spelling = "bq_initcap_delimiters($1, $2)"},
  };
  static constexpr StaticRule kSoundex[] = {{.arity = 1, .spelling = "bq_soundex($1)"}};
  static constexpr StaticRule kSafeConvertBytes[] = {
      {.arity = 1, .spelling = "bq_safe_convert_bytes_to_string($1)"},
  };
  // HOST, REG_DOMAIN and PUBLIC_SUFFIX follow GoogleSQL's copy of the public suffix list.
  static constexpr StaticRule kIpv4FromInt64[] = {
      {.arity = 1, .spelling = "bq_net_ipv4_from_int64($1)"},
  };
  static constexpr StaticRule kIpv4ToInt64[] = {
      {.arity = 1, .spelling = "bq_net_ipv4_to_int64($1)"},
  };
  static constexpr StaticRule kIpFromString[] = {
      {.arity = 1, .spelling = "bq_net_ip_from_string($1)"},
  };
  static constexpr StaticRule kSafeIpFromString[] = {
      {.arity = 1, .spelling = "bq_net_safe_ip_from_string($1)"},
  };
  static constexpr StaticRule kIpToString[] = {{.arity = 1, .spelling = "bq_net_ip_to_string($1)"}};
  static constexpr StaticRule kIpNetMask[] = {
      {.arity = 2, .spelling = "bq_net_ip_net_mask($1, $2)"},
  };
  static constexpr StaticRule kIpTrunc[] = {{.arity = 2, .spelling = "bq_net_ip_trunc($1, $2)"}};
  static constexpr StaticRule kHost[] = {{.arity = 1, .spelling = "bq_net_host($1)"}};
  static constexpr StaticRule kRegDomain[] = {{.arity = 1, .spelling = "bq_net_reg_domain($1)"}};
  static constexpr StaticRule kPublicSuffix[] = {
      {.arity = 1, .spelling = "bq_net_public_suffix($1)"},
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
  static constexpr StaticRule kAeadEncrypt[] = {
      {
          .arity = 3,
          .spelling = "bq_aead_encrypt($1, encode($2), encode($3))",
          .conditions = kBytesString,
      },
      {.arity = 3, .spelling = "bq_aead_encrypt($1, $2, $3)", .conditions = kBytesBytes},
  };
  static constexpr StaticRule kAeadDecryptBytes[] = {
      {
          .arity = 3,
          .spelling = "bq_aead_decrypt_bytes($1, $2, $3)",
          .conditions = ArgumentType<TYPE_BYTES>::kFirst,
      },
  };
  static constexpr StaticRule kAeadDecryptString[] = {
      {
          .arity = 3,
          .spelling = "bq_aead_decrypt_string($1, $2, encode($3))",
          .conditions = ArgumentType<TYPE_BYTES>::kFirst,
      },
  };
  static constexpr StaticRule kDeterministicEncrypt[] = {
      {
          .arity = 3,
          .spelling = "bq_deterministic_encrypt($1, encode($2), encode($3))",
          .conditions = kBytesString,
      },
      {.arity = 3, .spelling = "bq_deterministic_encrypt($1, $2, $3)", .conditions = kBytesBytes},
  };
  static constexpr StaticRule kDeterministicDecryptBytes[] = {
      {
          .arity = 3,
          .spelling = "bq_deterministic_decrypt_bytes($1, $2, $3)",
          .conditions = ArgumentType<TYPE_BYTES>::kFirst,
      },
  };
  static constexpr StaticRule kDeterministicDecryptString[] = {
      {
          .arity = 3,
          .spelling = "bq_deterministic_decrypt_string($1, $2, encode($3))",
          .conditions = ArgumentType<TYPE_BYTES>::kFirst,
      },
  };
  static constexpr StaticRule kNewKeyset[] = {{.arity = 1, .spelling = "bq_keys_new_keyset($1)"}};
  static constexpr StaticRule kRotateKeyset[] = {
      {.arity = 2, .spelling = "bq_keys_rotate_keyset($1, $2)"},
  };
  static constexpr StaticRule kKeysetLength[] = {
      {.arity = 1, .spelling = "bq_keys_keyset_length($1)"},
  };
  static constexpr StaticRule kKeysetToJson[] = {
      {.arity = 1, .spelling = "bq_keys_keyset_to_json($1)"},
  };
  static constexpr StaticRule kKeysetFromJson[] = {
      {.arity = 1, .spelling = "bq_keys_keyset_from_json($1)"},
  };
  static constexpr StaticRule kHllExtract[] = {
      {.arity = 1, .spelling = "bq_hll_count_extract($1)"},
  };
  // Without max_distance the distance is not capped.
  static constexpr auto kEditDistance = StringBytes(
      {2, 3}, "bq_edit_distance($1, $2, $3)", "bq_edit_distance_bytes($1, $2, $3)", kMaxLength);
  static constexpr auto kRegexpExtract =
      StringBytes({2, 4}, "bq_regexp_extract($1, $2, $3, $4)",
                  "bq_regexp_extract_bytes($1, $2, $3, $4)", kFirstOccurrence);
  static constexpr auto kRegexpInstr =
      StringBytes({2, 5}, "bq_regexp_instr($1, $2, $3, $4, $5)",
                  "bq_regexp_instr_bytes($1, $2, $3, $4, $5)", kFirstOccurrenceStart);
  // JSON goes to GoogleSQL as text.
  static constexpr StaticRule kParseJson[] = {
      {.arity = {1, 2}, .spelling = "json(bq_parse_json($1, $2))", .defaults = kExactDefault},
  };
  static constexpr StaticRule kBool[] = {Json(1, "bq_json_bool(CAST($1 AS VARCHAR))")};
  static constexpr StaticRule kString[] = {
      Json(1, "bq_json_string(CAST($1 AS VARCHAR))"),
      {
          .arity = {1, 2},
          .spelling = "bq_timestamp_string($1, $2)",
          .conditions = ArgumentType<TYPE_TIMESTAMP>::kFirst,
          .defaults = kUtcDefault,
      },
  };
  static constexpr StaticRule kInt64[] = {Json(1, "bq_json_int64(CAST($1 AS VARCHAR))")};
  static constexpr StaticRule kFloat64[] = {
      Json({1, 2}, "bq_json_float64(CAST($1 AS VARCHAR), $2)", kRoundDefault),
  };
  static constexpr StaticRule kJsonType[] = {
      {.arity = 1, .spelling = "bq_json_type(CAST($1 AS VARCHAR))"},
  };
  static constexpr StaticCondition kJsonField[] = {
      {.argument = 1, .types = ArgumentType<TYPE_JSON>::kTypes},
      {.argument = 2, .types = ArgumentType<TYPE_STRING>::kTypes},
  };
  static constexpr StaticCondition kJsonElement[] = {
      {.argument = 1, .types = ArgumentType<TYPE_JSON>::kTypes},
      {.argument = 2, .types = ArgumentType<TYPE_INT64>::kTypes},
  };
  static constexpr StaticRule kSubscript[] = {
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
  };
  static constexpr StaticRule kJsonFlatten[] = {
      {.arity = 1, .spelling = "CAST(bq_json_flatten(CAST($1 AS VARCHAR)) AS JSON[])"},
  };
  static constexpr StaticRule kLaxBool[] = {Json(1, "bq_lax_bool(CAST($1 AS VARCHAR))")};
  static constexpr StaticRule kLaxInt64[] = {Json(1, "bq_lax_int64(CAST($1 AS VARCHAR))")};
  static constexpr StaticRule kLaxFloat64[] = {Json(1, "bq_lax_float64(CAST($1 AS VARCHAR))")};
  static constexpr StaticRule kLaxString[] = {Json(1, "bq_lax_string(CAST($1 AS VARCHAR))")};
  static constexpr StaticRule kJsonKeys[] = {
      Json(3, "CAST(json(bq_json_keys(CAST($1 AS VARCHAR), $2, $3)) AS VARCHAR[])"),
  };
  static constexpr StaticRule kJsonStripNulls[] = {
      Json(4, "json(bq_json_strip_nulls(CAST($1 AS VARCHAR), $2, $3, $4))"),
  };
  static constexpr function_catalog::StaticNamedDefinition<StaticRule> kRules[] = {
      {"$INTERVAL", {.rules = kInterval, .uses = {TypeUse::kBuildsOrReadsInterval}}},
      {"MAKE_INTERVAL", {.rules = kMakeInterval, .uses = {TypeUse::kBuildsOrReadsInterval}}},
      {"JUSTIFY_HOURS", {.rules = kJustifyHours, .uses = {TypeUse::kBuildsOrReadsInterval}}},
      {"JUSTIFY_DAYS", {.rules = kJustifyDays, .uses = {TypeUse::kBuildsOrReadsInterval}}},
      {"JUSTIFY_INTERVAL", {.rules = kJustifyInterval, .uses = {TypeUse::kBuildsOrReadsInterval}}},
      {"PARSE_NUMERIC", {.rules = kParseNumeric}},
      {"PARSE_BIGNUMERIC", {.rules = kParseBigNumeric, .uses = {TypeUse::kBigNumeric}}},
      {"COSINE_DISTANCE", {.rules = kCosineDistance}},
      {"EUCLIDEAN_DISTANCE", {.rules = kEuclideanDistance}},
      {"CHR", {.rules = kChr}},
      {"SUBSTR", {.rules = kSubstr}},
      {"SUBSTRING", {.rules = kSubstr}},
      {"INSTR", {.rules = kInstr}},
      {"STRPOS", {.rules = kStrpos}},
      {"STARTS_WITH", {.rules = kStartsWith}},
      {"ENDS_WITH", {.rules = kEndsWith}},
      {"SPLIT", {.rules = kSplit}},
      {"GENERATE_DATE_ARRAY", {.rules = kGenerateDateArray}},
      {"FORMAT_DATE", {.rules = kFormatDateTime}},
      {"FORMAT_DATETIME", {.rules = kFormatDateTime}},
      {"FORMAT_TIMESTAMP", {.rules = kFormatDateTime}},
      {"FORMAT_TIME", {.rules = kFormatTime}},
      {"PARSE_DATE", {.rules = kParseDate}},
      {"PARSE_DATETIME", {.rules = kParseDateTime}},
      {"PARSE_TIME", {.rules = kParseTime}},
      {"PARSE_TIMESTAMP", {.rules = kParseTimestamp}},
      {"NORMALIZE", {.rules = kNormalize}},
      {"NORMALIZE_AND_CASEFOLD", {.rules = kNormalizeCasefold}},
      {"SHA512", {.rules = kSha512}},
      {"FARM_FINGERPRINT", {.rules = kFarmFingerprint}},
      {"TO_BASE32", {.rules = kToBase32}},
      {"FROM_BASE32", {.rules = kFromBase32}},
      {"INITCAP", {.rules = kInitcap}},
      {"SOUNDEX", {.rules = kSoundex}},
      {"SAFE_CONVERT_BYTES_TO_STRING", {.rules = kSafeConvertBytes}},
      {"NET.IPV4_FROM_INT64", {.rules = kIpv4FromInt64}},
      {"NET.IPV4_TO_INT64", {.rules = kIpv4ToInt64}},
      {"NET.IP_FROM_STRING", {.rules = kIpFromString}},
      {"NET.SAFE_IP_FROM_STRING", {.rules = kSafeIpFromString}},
      {"NET.IP_TO_STRING", {.rules = kIpToString}},
      {"NET.IP_NET_MASK", {.rules = kIpNetMask}},
      {"NET.IP_TRUNC", {.rules = kIpTrunc}},
      {"NET.HOST", {.rules = kHost}},
      {"NET.REG_DOMAIN", {.rules = kRegDomain}},
      {"NET.PUBLIC_SUFFIX", {.rules = kPublicSuffix}},
      {"AEAD.ENCRYPT", {.rules = kAeadEncrypt}},
      {"AEAD.DECRYPT_BYTES", {.rules = kAeadDecryptBytes}},
      {"AEAD.DECRYPT_STRING", {.rules = kAeadDecryptString}},
      {"DETERMINISTIC_ENCRYPT", {.rules = kDeterministicEncrypt}},
      {"DETERMINISTIC_DECRYPT_BYTES", {.rules = kDeterministicDecryptBytes}},
      {"DETERMINISTIC_DECRYPT_STRING", {.rules = kDeterministicDecryptString}},
      {"KEYS.NEW_KEYSET", {.rules = kNewKeyset}},
      {"KEYS.ROTATE_KEYSET", {.rules = kRotateKeyset}},
      {"KEYS.KEYSET_LENGTH", {.rules = kKeysetLength}},
      {"KEYS.KEYSET_TO_JSON", {.rules = kKeysetToJson}},
      {"KEYS.KEYSET_FROM_JSON", {.rules = kKeysetFromJson}},
      {"HLL_COUNT.EXTRACT", {.rules = kHllExtract}},
      {"EDIT_DISTANCE", {.rules = kEditDistance}},
      {"REGEXP_EXTRACT", {.rules = kRegexpExtract}},
      {"REGEXP_INSTR", {.rules = kRegexpInstr}},
      {"PARSE_JSON", {.rules = kParseJson}},
      {"BOOL", {.rules = kBool}},
      {"STRING", {.rules = kString}},
      {"INT64", {.rules = kInt64}},
      {"FLOAT64", {.rules = kFloat64}},
      {"DOUBLE", {.rules = kFloat64}},
      {"JSON_TYPE", {.rules = kJsonType}},
      {"$SUBSCRIPT", {.rules = kSubscript}},
      {"JSON_FLATTEN", {.rules = kJsonFlatten}},
      {"LAX_BOOL", {.rules = kLaxBool}},
      {"LAX_INT64", {.rules = kLaxInt64}},
      {"LAX_FLOAT64", {.rules = kLaxFloat64}},
      {"LAX_DOUBLE", {.rules = kLaxFloat64}},
      {"LAX_STRING", {.rules = kLaxString}},
      {"JSON_KEYS", {.rules = kJsonKeys, .uses = {TypeUse::kRejectsSafe}}},
      {"JSON_STRIP_NULLS", {.rules = kJsonStripNulls}},
  };
  return kRules;
}

// Only rules whose builders generate SQL strings need runtime-owned storage.
std::span<const function_catalog::NamedDefinition<Rule>> BackendRules() {
  static const function_catalog::NamedDefinition<Rule> kRules[] = {
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
      // DuckDB's BLOB has almost no functions; its Unicode and error semantics differ too.
      {"REPEAT", {.rules = Strings("bq_repeat", 2)}},
      {"LOWER", {.rules = Strings("bq_lower", 1)}},
      {"UPPER", {.rules = Strings("bq_upper", 1)}},
      {"REVERSE", {.rules = Strings("bq_reverse", 1)}},
      {"REPLACE", {.rules = Strings("bq_replace", 3)}},
      {"TRANSLATE", {.rules = Strings("bq_translate", 3)}},
      {"ASCII", {.rules = Strings("bq_ascii", 1)}},
      {"UNICODE", {.rules = Same("bq_unicode", 1, {Is(1, {TYPE_STRING})})}},
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
          "GENERATE_RANGE_ARRAY",
          {
              .rules = GenerateRangeArray(),
              .uses = {TypeUse::kBuildsOrReadsInterval, TypeUse::kBuildsOrReadsRange},
          },
      },
      // DuckDB's regexp replacement and nonparticipating captures have different semantics.
      {"REGEXP_CONTAINS", {.rules = Strings("bq_regexp_contains", 2)}},
      {"REGEXP_REPLACE", {.rules = Strings("bq_regexp_replace", 3)}},
      {"REGEXP_EXTRACT_ALL", {.rules = Strings("bq_regexp_extract_all", 2)}},
      {"JSON_QUERY", {.rules = JsonExtract("bq_json_query", true, true)}},
      {"JSON_EXTRACT", {.rules = JsonExtract("bq_json_query", false, true)}},
      {"JSON_VALUE", {.rules = JsonExtract("bq_json_value", true, false)}},
      {"JSON_EXTRACT_SCALAR", {.rules = JsonExtract("bq_json_value", false, false)}},
      {"JSON_QUERY_ARRAY", {.rules = JsonExtract("bq_json_query_array", true, false)}},
      {"JSON_EXTRACT_ARRAY", {.rules = JsonExtract("bq_json_query_array", false, false)}},
      {"JSON_VALUE_ARRAY", {.rules = JsonExtract("bq_json_value_array", true, false)}},
      {"JSON_EXTRACT_STRING_ARRAY", {.rules = JsonExtract("bq_json_value_array", false, false)}},
  };
  return kRules;
}

}  // namespace bigquery_emulator_duckdb::translator
