#include "googlesql/public/functions/string.h"

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/ascii.h"
#include "absl/strings/string_view.h"
#include "duckdb.h"
#include "googlesql/public/functions/convert_string.h"
#include "googlesql/public/functions/distance.h"
#include "googlesql/public/functions/hash.h"
#include "googlesql/public/functions/net.h"
#include "googlesql/public/functions/normalize_mode.pb.h"
#include "googlesql/public/functions/regexp.h"
#include "googlesql/public/numeric_value.h"
#include "src/backend_functions/register.h"
#include "src/backend_functions/scalar.h"
#include "src/backend_functions/string.h"
#include "src/duckdb_handle.h"

namespace bigquery_emulator_duckdb::backend_functions {
namespace {

// The parameters of a GoogleSQL string function: its arguments, then an out parameter and an
// error.
template <typename Function>
struct Signature;

template <typename... Parameters>
struct Signature<bool (*)(Parameters...)> {
  // Used by Out and make_index_sequence; Cppcheck misses these template arguments.
  // cppcheck-suppress unusedStructMember
  static constexpr std::size_t kArity = sizeof...(Parameters) - 2;
  template <std::size_t I>
  using Parameter = std::tuple_element_t<I, std::tuple<Parameters...>>;
  using Out = std::remove_pointer_t<Parameter<kArity>>;
};

// A STRING or BYTES argument as a string_view parameter, or an INT64 one.
template <typename T>
auto Argument(const Arguments& arguments, idx_t column) {
  if constexpr (std::is_same_v<T, int64_t>) {
    return arguments.Int(column);
  } else {
    return arguments.String(column);
  }
}

// A string_view result points into the arguments, which do not outlive the row.
template <typename T>
auto Owned(T value) {
  if constexpr (std::is_same_v<T, absl::string_view>) {
    return std::string(value);
  } else {
    return value;
  }
}

// The GoogleSQL function `kFunction` of STRING, BYTES and INT64 arguments, such as LowerUtf8,
// taking the columns in order.
template <auto kFunction>
void Apply(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  using S = Signature<decltype(kFunction)>;
  EachRow(info, input, output, [](const Arguments& arguments) {
    return [&]<std::size_t... I>(std::index_sequence<I...>) {
      const std::tuple values{Argument<typename S::template Parameter<I>>(arguments, I)...};
      typename S::Out out{};
      absl::Status error;
      const bool ok = kFunction(std::get<I>(values)..., &out, &error);
      return ToStatusOr(ok, Owned(std::move(out)), error);
    }(std::make_index_sequence<S::kArity>());
  });
}

// NORMALIZE and NORMALIZE_AND_CASEFOLD, with the mode by its name, such as NFKC.
template <bool kCasefold>
void Normalize(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  EachRow(info, input, output, [](const Arguments& arguments) -> absl::StatusOr<std::string> {
    googlesql::functions::NormalizeMode mode{};
    if (!googlesql::functions::NormalizeMode_Parse(arguments.String(1), &mode)) {
      return absl::OutOfRangeError("Invalid normalize mode");
    }
    std::string out;
    absl::Status error;
    const bool ok =
        googlesql::functions::Normalize(arguments.String(0), mode, kCasefold, &out, &error);
    return ToStatusOr(ok, out, error);
  });
}

void FarmFingerprint(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  EachRow(info, input, output, [](const Arguments& arguments) -> absl::StatusOr<int64_t> {
    return googlesql::functions::FarmFingerprint(arguments.String(0));
  });
}

void Sha512(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  const auto hasher = googlesql::functions::Hasher::Create(googlesql::functions::Hasher::kSha512);
  EachRow(info, input, output,
          [&hasher](const Arguments& arguments) -> absl::StatusOr<std::string> {
            return hasher->Hash(arguments.String(0));
          });
}

// RFC 4648's base32, which BigQuery's TO_BASE32 and FROM_BASE32 implement and GoogleSQL leaves
// out of its open source.
constexpr std::string_view kBase32Alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";

void ToBase32(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  EachRow(info, input, output, [](const Arguments& arguments) -> absl::StatusOr<std::string> {
    const std::string bytes = arguments.String(0);
    std::string out;
    uint32_t buffer = 0;
    unsigned bits = 0;
    for (const char byte : bytes) {
      buffer = (buffer << 8U) | static_cast<uint8_t>(byte);
      for (bits += 8; bits >= 5U; bits -= 5) {
        out += kBase32Alphabet.at((buffer >> (bits - 5U)) & 31U);
      }
    }
    if (bits > 0) {
      out += kBase32Alphabet.at((buffer << (5U - bits)) & 31U);
    }
    out.resize((out.size() + 7) / 8 * 8, '=');
    return out;
  });
}

// Accepts either case, and text either padded to groups of 8 characters or not padded at all.
void FromBase32(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  EachRow(info, input, output, [](const Arguments& arguments) -> absl::StatusOr<std::string> {
    const std::string text = arguments.String(0);
    const std::string_view data(text.data(), text.find_last_not_of('=') + 1);
    // A final group of 1 to 4 bytes has 2, 4, 5 or 7 characters before its padding.
    const std::size_t tail = data.size() % 8;
    const bool padded = text.size() != data.size();
    if ((padded && (text.size() % 8 != 0 || text.size() - data.size() >= 8)) || tail == 1 ||
        tail == 3 || tail == 6) {
      return absl::OutOfRangeError("Failed to decode invalid base32 string");
    }
    std::string out;
    uint32_t buffer = 0;
    unsigned bits = 0;
    for (const char c : data) {
      const std::size_t value = kBase32Alphabet.find(absl::ascii_toupper(c));
      if (value == std::string_view::npos) {
        return absl::OutOfRangeError("Failed to decode invalid base32 string");
      }
      buffer = (buffer << 5U) | value;
      bits += 5;
      if (bits >= 8) {
        bits -= 8;
        out += static_cast<char>(buffer >> bits);
      }
    }
    return out;
  });
}

void InitCap(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  EachRow(info, input, output, [input](const Arguments& arguments) {
    std::string out;
    absl::Status error;
    const bool ok =
        duckdb_data_chunk_get_column_count(input) == 1
            ? googlesql::functions::InitialCapitalizeDefault(arguments.String(0), &out, &error)
            : googlesql::functions::InitialCapitalize(arguments.String(0), arguments.String(1),
                                                      &out, &error);
    return ToStatusOr(ok, out, error);
  });
}

template <bool kBytes>
void EditDistance(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  EachRow(info, input, output, [](const Arguments& arguments) {
    return (kBytes ? googlesql::functions::EditDistanceBytes : googlesql::functions::EditDistance)(
        arguments.String(0), arguments.String(1), arguments.Int(2));
  });
}

// Sets each row to what `compute` returns for the row's arguments and its regular expression,
// the second argument, which GoogleSQL compiles as UTF-8 for STRING or as bytes for BYTES.
template <bool kBytes, typename Compute>
void WithRegExp(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output,
                Compute compute) {
  EachRow(info, input, output, [&compute](const Arguments& arguments) {
    const std::string pattern = arguments.String(1);
    const auto regexp = kBytes ? googlesql::functions::MakeRegExpBytes(pattern)
                               : googlesql::functions::MakeRegExpUtf8(pattern);
    using Result = decltype(compute(**regexp, arguments));
    if (!regexp.ok()) {
      return Result(regexp.status());
    }
    return compute(**regexp, arguments);
  });
}

constexpr googlesql::functions::RegExp::PositionUnit PositionUnit(bool bytes) {
  return bytes ? googlesql::functions::RegExp::kBytes : googlesql::functions::RegExp::kUtf8Chars;
}

template <bool kBytes>
void RegexpContains(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  WithRegExp<kBytes>(info, input, output,
                     [](const googlesql::functions::RegExp& regexp, const Arguments& arguments) {
                       bool out = false;
                       absl::Status error;
                       const bool ok = regexp.Contains(arguments.String(0), &out, &error);
                       return ToStatusOr(ok, out, error);
                     });
}

// REGEXP_REPLACE(source, regexp, replacement).
template <bool kBytes>
void RegexpReplace(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  WithRegExp<kBytes>(info, input, output,
                     [](const googlesql::functions::RegExp& regexp, const Arguments& arguments) {
                       std::string out;
                       absl::Status error;
                       const bool ok =
                           regexp.Replace(arguments.String(0), arguments.String(2), &out, &error);
                       return ToStatusOr(ok, out, error);
                     });
}

// REGEXP_EXTRACT(source, regexp, position, occurrence), which is NULL without a match or when
// the capturing group takes no part in it.
template <bool kBytes>
void RegexpExtract(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  WithRegExp<kBytes>(
      info, input, output,
      [](const googlesql::functions::RegExp& regexp,
         const Arguments& arguments) -> absl::StatusOr<std::optional<std::string>> {
        const std::string source = arguments.String(0);
        absl::string_view out;
        bool is_null = true;
        absl::Status error;
        if (!regexp.Extract(source, PositionUnit(kBytes), arguments.Int(2), arguments.Int(3),
                            /*use_legacy_position_behavior=*/false, &out, &is_null, &error)) {
          return error;
        }
        if (is_null) {
          return std::nullopt;
        }
        return std::string(out);
      });
}

// REGEXP_EXTRACT_ALL(source, regexp).
template <bool kBytes>
void RegexpExtractAll(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  WithRegExp<kBytes>(info, input, output,
                     [](const googlesql::functions::RegExp& regexp,
                        const Arguments& arguments) -> absl::StatusOr<std::vector<std::string>> {
                       const std::string source = arguments.String(0);
                       auto matches = regexp.CreateExtractAllIterator(source);
                       std::vector<std::string> out;
                       absl::string_view match;
                       absl::Status error;
                       while (matches.Next(&match, &error)) {
                         out.emplace_back(match);
                       }
                       if (!error.ok()) {
                         return error;
                       }
                       return out;
                     });
}

// REGEXP_INSTR(source, regexp, position, occurrence, occurrence_position).
template <bool kBytes>
void RegexpInstr(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  WithRegExp<kBytes>(
      info, input, output,
      [](const googlesql::functions::RegExp& regexp,
         const Arguments& arguments) -> absl::StatusOr<int64_t> {
        const int64_t occurrence_position = arguments.Int(4);
        if (occurrence_position != 0 && occurrence_position != 1) {
          return absl::OutOfRangeError(
              "Invalid return_position_after_match; it must be 0 or 1 (in REGEXP_INSTR)");
        }
        const std::string source = arguments.String(0);
        int64_t out = 0;
        absl::Status error;
        const bool ok = regexp.Instr(
            {
                .input_str = source,
                .position_unit = PositionUnit(kBytes),
                .position = arguments.Int(2),
                .occurrence_index = arguments.Int(3),
                .return_position = occurrence_position == 0
                                       ? googlesql::functions::RegExp::kStartOfMatch
                                       : googlesql::functions::RegExp::kEndOfMatch,
                .out = &out,
            },
            /*use_legacy_position_behavior=*/false, &error);
        return ToStatusOr(ok, out, error);
      });
}

// The text of a DECIMAL, which DuckDB pads with zeros to its scale, as BigQuery writes a NUMERIC
// or BIGNUMERIC, without trailing fractional zeros. Every DECIMAL fits a BIGNUMERIC.
void DecimalString(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  EachRow(info, input, output, [](const Arguments& arguments) -> absl::StatusOr<std::string> {
    const auto value = googlesql::BigNumericValue::FromString(arguments.String(0));
    if (!value.ok()) {
      return value.status();
    }
    return value->ToString();
  });
}

// The text of a FLOAT64 as BigQuery writes it, the shortest that reads back as the same value,
// without DuckDB's ".0" for a whole number, preserving the sign of negative zero.
void DoubleString(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  EachRow(info, input, output, [](const Arguments& arguments) {
    std::string out;
    absl::Status error;
    const bool ok = googlesql::functions::NumericToString(arguments.Double(0), &out, &error,
                                                          /*canonicalize_zero=*/false);
    return ToStatusOr(ok, out, error);
  });
}

// NET.HOST, NET.REG_DOMAIN and NET.PUBLIC_SUFFIX, which are NULL for a URL without the part.
template <absl::Status (*kFunction)(absl::string_view, absl::string_view*, bool*)>
void UrlPart(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  EachRow(info, input, output,
          [](const Arguments& arguments) -> absl::StatusOr<std::optional<std::string>> {
            const std::string url = arguments.String(0);
            absl::string_view out;
            bool is_null = true;
            if (absl::Status status = kFunction(url, &out, &is_null); !status.ok()) {
              return status;
            }
            if (is_null) {
              return std::nullopt;
            }
            return std::string(out);
          });
}

// NET.SAFE_IP_FROM_STRING, which is NULL for an invalid address.
void SafeIpFromString(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  EachRow(info, input, output,
          [](const Arguments& arguments) -> absl::StatusOr<std::optional<std::string>> {
            std::string out;
            bool is_null = true;
            if (absl::Status status = googlesql::functions::net::SafeIPFromString(
                    arguments.String(0), &out, &is_null);
                !status.ok()) {
              return status;
            }
            if (is_null) {
              return std::nullopt;
            }
            return out;
          });
}

}  // namespace

void RegisterStringFunctions(duckdb_connection connection) {
  namespace fn = googlesql::functions;
  // String functions, where DuckDB differs in case mapping, whitespace, errors and limits, or
  // has no BYTES overload.
  for (const auto& [name, parameters, result, function] :
       std::initializer_list<std::tuple<const char*, std::vector<duckdb_type>, duckdb_type,
                                        duckdb_scalar_function_t>>{
           {"bq_repeat", {kVarchar, kBigint}, kVarchar, Apply<fn::Repeat>},
           {"bq_repeat_bytes", {kBlob, kBigint}, kBlob, Apply<fn::Repeat>},
           {"bq_lower", {kVarchar}, kVarchar, Apply<fn::LowerUtf8>},
           {"bq_lower_bytes", {kBlob}, kBlob, Apply<fn::LowerBytes>},
           {"bq_upper", {kVarchar}, kVarchar, Apply<fn::UpperUtf8>},
           {"bq_upper_bytes", {kBlob}, kBlob, Apply<fn::UpperBytes>},
           {"bq_reverse", {kVarchar}, kVarchar, Apply<fn::ReverseUtf8>},
           {"bq_reverse_bytes", {kBlob}, kBlob, Apply<fn::ReverseBytes>},
           {"bq_replace", {kVarchar, kVarchar, kVarchar}, kVarchar, Apply<fn::ReplaceUtf8>},
           {"bq_replace_bytes", {kBlob, kBlob, kBlob}, kBlob, Apply<fn::ReplaceBytes>},
           {"bq_translate", {kVarchar, kVarchar, kVarchar}, kVarchar, Apply<fn::TranslateUtf8>},
           {"bq_translate_bytes", {kBlob, kBlob, kBlob}, kBlob, Apply<fn::TranslateBytes>},
           {"bq_starts_with_bytes", {kBlob, kBlob}, kBoolean, Apply<fn::StartsWithBytes>},
           {"bq_ends_with_bytes", {kBlob, kBlob}, kBoolean, Apply<fn::EndsWithBytes>},
           {"bq_ascii", {kVarchar}, kBigint, Apply<fn::FirstCharOfStringToASCII>},
           {"bq_ascii_bytes", {kBlob}, kBigint, Apply<fn::FirstByteOfBytesToASCII>},
           {
               "bq_instr",
               {kVarchar, kVarchar, kBigint, kBigint},
               kBigint,
               Apply<fn::StrPosOccurrenceUtf8>,
           },
           {
               "bq_instr_bytes",
               {kBlob, kBlob, kBigint, kBigint},
               kBigint,
               Apply<fn::StrPosOccurrenceBytes>,
           },
           {"bq_left", {kVarchar, kBigint}, kVarchar, Apply<fn::LeftUtf8>},
           {"bq_left_bytes", {kBlob, kBigint}, kBlob, Apply<fn::LeftBytes>},
           {"bq_right", {kVarchar, kBigint}, kVarchar, Apply<fn::RightUtf8>},
           {"bq_right_bytes", {kBlob, kBigint}, kBlob, Apply<fn::RightBytes>},
           {"bq_substr_bytes", {kBlob, kBigint, kBigint}, kBlob, Apply<fn::SubstrWithLengthBytes>},
           {"bq_lpad", {kVarchar, kBigint, kVarchar}, kVarchar, Apply<fn::LeftPadUtf8>},
           {"bq_lpad_bytes", {kBlob, kBigint, kBlob}, kBlob, Apply<fn::LeftPadBytes>},
           {"bq_rpad", {kVarchar, kBigint, kVarchar}, kVarchar, Apply<fn::RightPadUtf8>},
           {"bq_rpad_bytes", {kBlob, kBigint, kBlob}, kBlob, Apply<fn::RightPadBytes>},
           {"bq_trim", {kVarchar}, kVarchar, Apply<fn::TrimSpacesUtf8>},
           {"bq_trim_chars", {kVarchar, kVarchar}, kVarchar, Apply<fn::TrimUtf8>},
           {"bq_trim_bytes", {kBlob, kBlob}, kBlob, Apply<fn::TrimBytes>},
           {"bq_ltrim", {kVarchar}, kVarchar, Apply<fn::LeftTrimSpacesUtf8>},
           {"bq_ltrim_chars", {kVarchar, kVarchar}, kVarchar, Apply<fn::LeftTrimUtf8>},
           {"bq_ltrim_bytes", {kBlob, kBlob}, kBlob, Apply<fn::LeftTrimBytes>},
           {"bq_rtrim", {kVarchar}, kVarchar, Apply<fn::RightTrimSpacesUtf8>},
           {"bq_rtrim_chars", {kVarchar, kVarchar}, kVarchar, Apply<fn::RightTrimUtf8>},
           {"bq_rtrim_bytes", {kBlob, kBlob}, kBlob, Apply<fn::RightTrimBytes>},
           {"bq_normalize", {kVarchar, kVarchar}, kVarchar, Normalize<false>},
           {"bq_normalize_and_casefold", {kVarchar, kVarchar}, kVarchar, Normalize<true>},
           {"bq_soundex", {kVarchar}, kVarchar, Apply<fn::Soundex>},
           {"bq_safe_convert_bytes_to_string", {kBlob}, kVarchar, Apply<fn::SafeConvertBytes>},
           // NET functions. IP addresses are BYTES in network byte order.
           {"bq_net_ipv4_from_int64", {kBigint}, kBlob, Apply<fn::net::IPv4FromInt64>},
           {"bq_net_ipv4_to_int64", {kBlob}, kBigint, Apply<fn::net::IPv4ToInt64>},
           {"bq_net_ip_from_string", {kVarchar}, kBlob, Apply<fn::net::IPFromString>},
           {"bq_net_safe_ip_from_string", {kVarchar}, kBlob, SafeIpFromString},
           {"bq_net_ip_to_string", {kBlob}, kVarchar, Apply<fn::net::IPToString>},
           {"bq_net_ip_net_mask", {kBigint, kBigint}, kBlob, Apply<fn::net::IPNetMask>},
           {"bq_net_ip_trunc", {kBlob, kBigint}, kBlob, Apply<fn::net::IPTrunc>},
           {"bq_net_host", {kVarchar}, kVarchar, UrlPart<fn::net::Host>},
           {"bq_net_reg_domain", {kVarchar}, kVarchar, UrlPart<fn::net::RegDomain>},
           {"bq_net_public_suffix", {kVarchar}, kVarchar, UrlPart<fn::net::PublicSuffix>},
       }) {
    Register(connection, name, parameters, result, function);
  }
  {
    LogicalType const blob(duckdb_create_logical_type(kBlob));
    LogicalType const list(duckdb_create_list_type(blob.get()));
    Register(
        connection, "bq_split_bytes", {kBlob, kBlob}, list.get(),
        Apply<static_cast<bool (*)(absl::string_view, absl::string_view, std::vector<std::string>*,
                                   absl::Status*)>(fn::SplitBytes)>);
  }
  Register(connection, "bq_farm_fingerprint", {kBlob}, kBigint, FarmFingerprint);
  Register(connection, "bq_sha512", {kBlob}, kBlob, Sha512);
  Register(connection, "bq_to_base32", {kBlob}, kVarchar, ToBase32);
  Register(connection, "bq_from_base32", {kVarchar}, kBlob, FromBase32);
  Register(connection, "bq_initcap", {kVarchar}, kVarchar, InitCap);
  Register(connection, "bq_decimal_string", {kVarchar}, kVarchar, DecimalString);
  Register(connection, "bq_double_string", {kDouble}, kVarchar, DoubleString);
  Register(connection, "bq_initcap_delimiters", {kVarchar, kVarchar}, kVarchar, InitCap);
  Register(connection, "bq_edit_distance", {kVarchar, kVarchar, kBigint}, kBigint,
           EditDistance<false>);
  Register(connection, "bq_edit_distance_bytes", {kBlob, kBlob, kBigint}, kBigint,
           EditDistance<true>);
  Register(connection, "bq_regexp_instr", {kVarchar, kVarchar, kBigint, kBigint, kBigint}, kBigint,
           RegexpInstr<false>);
  Register(connection, "bq_regexp_instr_bytes", {kBlob, kBlob, kBigint, kBigint, kBigint}, kBigint,
           RegexpInstr<true>);
  Register(connection, "bq_regexp_contains", {kVarchar, kVarchar}, kBoolean, RegexpContains<false>);
  Register(connection, "bq_regexp_contains_bytes", {kBlob, kBlob}, kBoolean, RegexpContains<true>);
  Register(connection, "bq_regexp_replace", {kVarchar, kVarchar, kVarchar}, kVarchar,
           RegexpReplace<false>);
  Register(connection, "bq_regexp_replace_bytes", {kBlob, kBlob, kBlob}, kBlob,
           RegexpReplace<true>);
  Register(connection, "bq_regexp_extract", {kVarchar, kVarchar, kBigint, kBigint}, kVarchar,
           RegexpExtract<false>);
  Register(connection, "bq_regexp_extract_bytes", {kBlob, kBlob, kBigint, kBigint}, kBlob,
           RegexpExtract<true>);
  for (const auto& [name, type, function] :
       std::initializer_list<std::tuple<const char*, duckdb_type, duckdb_scalar_function_t>>{
           {"bq_regexp_extract_all", kVarchar, RegexpExtractAll<false>},
           {"bq_regexp_extract_all_bytes", kBlob, RegexpExtractAll<true>},
       }) {
    LogicalType const element(duckdb_create_logical_type(type));
    LogicalType const list(duckdb_create_list_type(element.get()));
    Register(connection, name, {type, type}, list.get(), function);
  }
}

}  // namespace bigquery_emulator_duckdb::backend_functions
