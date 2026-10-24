#include "googlesql/public/functions/string.h"

#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "duckdb.h"
#include "googlesql/public/functions/distance.h"
#include "googlesql/public/functions/hash.h"
#include "googlesql/public/functions/net.h"
#include "googlesql/public/functions/regexp.h"
#include "src/duckdb_functions/apply.h"
#include "src/duckdb_functions/register.h"
#include "src/duckdb_functions/scalar.h"
#include "src/duckdb_handle.h"
#include "src/googlesql_functions/string.h"
#include "src/googlesql_functions_duckdb/string.h"

namespace bigquery_emulator_duckdb::functions_duckdb {
using namespace duckdb_functions;
using namespace googlesql_functions;
namespace {

void Sha512(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  const auto hasher = googlesql::functions::Hasher::Create(googlesql::functions::Hasher::kSha512);
  EachRow(info, input, output,
          [&hasher](const Arguments& arguments) -> absl::StatusOr<std::string> {
            return hasher->Hash(arguments.String(0));
          });
}

template <auto kFunction>
absl::StatusOr<int64_t> EditDistance(std::string_view first, std::string_view second,
                                     int64_t max_distance) {
  return kFunction(first, second, max_distance);
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

}  // namespace

void RegisterStringFunctions(duckdb_connection connection) {
  namespace fn = googlesql::functions;
  // String functions, where DuckDB differs in case mapping, whitespace, errors and limits, or
  // has no BYTES overload.
  for (const auto& [name, parameters, result, function] :
       std::initializer_list<std::tuple<const char*, std::vector<duckdb_type>, duckdb_type,
                                        duckdb_scalar_function_t>>{
           {
               "bq_check_string_length",
               {kVarchar, kBigint, kVarchar},
               kBoolean,
               Apply<googlesql_functions::CheckStringLength>,
           },
           {
               "bq_check_bytes_length",
               {kBlob, kBigint, kVarchar},
               kBoolean,
               Apply<googlesql_functions::CheckBytesLength>,
           },
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
           {"bq_unicode", {kVarchar}, kBigint, Apply<fn::FirstCharToCodePoint>},
           {"bq_chr", {kBigint}, kVarchar, Apply<Chr>},
           {"bq_left", {kVarchar, kBigint}, kVarchar, Apply<fn::LeftUtf8>},
           {"bq_left_bytes", {kBlob, kBigint}, kBlob, Apply<fn::LeftBytes>},
           {"bq_right", {kVarchar, kBigint}, kVarchar, Apply<fn::RightUtf8>},
           {"bq_right_bytes", {kBlob, kBigint}, kBlob, Apply<fn::RightBytes>},
           {"bq_substr", {kVarchar, kBigint, kBigint}, kVarchar, Apply<fn::SubstrWithLengthUtf8>},
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
           {"bq_normalize", {kVarchar, kVarchar}, kVarchar, Apply<googlesql_functions::Normalize>},
           {
               "bq_normalize_and_casefold",
               {kVarchar, kVarchar},
               kVarchar,
               Apply<googlesql_functions::NormalizeAndCasefold>,
           },
           {"bq_soundex", {kVarchar}, kVarchar, Apply<fn::Soundex>},
           {"bq_safe_convert_bytes_to_string", {kBlob}, kVarchar, Apply<fn::SafeConvertBytes>},
           // NET functions. IP addresses are BYTES in network byte order.
           {"bq_net_ipv4_from_int64", {kBigint}, kBlob, Apply<fn::net::IPv4FromInt64>},
           {"bq_net_ipv4_to_int64", {kBlob}, kBigint, Apply<fn::net::IPv4ToInt64>},
           {"bq_net_ip_from_string", {kVarchar}, kBlob, Apply<fn::net::IPFromString>},
           {"bq_net_safe_ip_from_string", {kVarchar}, kBlob, Apply<fn::net::SafeIPFromString>},
           {"bq_net_ip_to_string", {kBlob}, kVarchar, Apply<fn::net::IPToString>},
           {"bq_net_ip_net_mask", {kBigint, kBigint}, kBlob, Apply<fn::net::IPNetMask>},
           {"bq_net_ip_trunc", {kBlob, kBigint}, kBlob, Apply<fn::net::IPTrunc>},
           {"bq_net_host", {kVarchar}, kVarchar, Apply<fn::net::Host>},
           {"bq_net_reg_domain", {kVarchar}, kVarchar, Apply<fn::net::RegDomain>},
           {"bq_net_public_suffix", {kVarchar}, kVarchar, Apply<fn::net::PublicSuffix>},
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
  {
    LogicalType const bigint(duckdb_create_logical_type(kBigint));
    LogicalType const list(duckdb_create_list_type(bigint.get()));
    Register(connection, "bq_to_code_points", {kVarchar}, list.get(),
             Apply<fn::StringToCodePoints>);
    Register(connection, "bq_to_code_points_bytes", {kBlob}, list.get(),
             Apply<fn::BytesToCodePoints>);
    LogicalType const varchar(duckdb_create_logical_type(kVarchar));
    LogicalType const blob(duckdb_create_logical_type(kBlob));
    Register(connection, "bq_code_points_to_string", {list.get()}, varchar.get(),
             Apply<fn::CodePointsToString>);
    Register(connection, "bq_code_points_to_bytes", {list.get()}, blob.get(),
             Apply<fn::CodePointsToBytes>);
  }
  Register(connection, "bq_farm_fingerprint", {kBlob}, kBigint, Apply<fn::FarmFingerprint>);
  Register(connection, "bq_sha512", {kBlob}, kBlob, Sha512);
  Register(connection, "bq_to_base32", {kBlob}, kVarchar, Apply<ToBase32>);
  Register(connection, "bq_from_base32", {kVarchar}, kBlob, Apply<FromBase32>);
  Register(connection, "bq_initcap", {kVarchar}, kVarchar, Apply<fn::InitialCapitalizeDefault>);
  Register(connection, "bq_decimal_string", {kVarchar}, kVarchar, Apply<DecimalString>);
  Register(connection, "bq_double_string", {kDouble}, kVarchar, Apply<DoubleString>);
  Register(connection, "bq_initcap_delimiters", {kVarchar, kVarchar}, kVarchar,
           Apply<fn::InitialCapitalize>);
  Register(connection, "bq_edit_distance", {kVarchar, kVarchar, kBigint}, kBigint,
           Apply<EditDistance<fn::EditDistance>>);
  Register(connection, "bq_edit_distance_bytes", {kBlob, kBlob, kBigint}, kBigint,
           Apply<EditDistance<fn::EditDistanceBytes>>);
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

}  // namespace bigquery_emulator_duckdb::functions_duckdb
