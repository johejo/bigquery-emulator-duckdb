#include "src/googlesql_functions/string.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/ascii.h"
#include "googlesql/base/status_macros.h"
#include "googlesql/public/functions/convert_string.h"
#include "googlesql/public/functions/normalize_mode.pb.h"
#include "googlesql/public/functions/string.h"
#include "googlesql/public/numeric_value.h"
#include "googlesql/public/type.pb.h"
#include "googlesql/public/type_parameters.pb.h"
#include "googlesql/public/types/type_parameters.h"
#include "googlesql/public/value.h"
#include "googlesql/reference_impl/type_parameter_constraints.h"

namespace googlesql_functions {

namespace {

// Keep the character count, range validation and error text of parameterized STRING/BYTES
// columns in GoogleSQL. BigQuery reports the top-level column even for a nested length error.
template <bool kBytes>
absl::StatusOr<bool> CheckLength(std::string_view text, int64_t maximum, std::string_view column) {
  googlesql::StringTypeParametersProto proto;
  proto.set_max_length(maximum);
  GOOGLESQL_ASSIGN_OR_RETURN(const auto parameters,
                             googlesql::TypeParameters::MakeStringTypeParameters(proto));
  auto value = kBytes ? googlesql::Value::Bytes(text) : googlesql::Value::String(text);
  const auto status = googlesql::ApplyConstraints(parameters, googlesql::PRODUCT_EXTERNAL, value);
  if (!status.ok()) {
    return absl::OutOfRangeError((column.empty() ? "" : "Field " + std::string(column) + ": ") +
                                 std::string(status.message()));
  }
  return true;
  // Value releases its StringRef in its out-of-line destructor.
  // NOLINTNEXTLINE(clang-analyzer-cplusplus.NewDeleteLeaks)
}

// NORMALIZE and NORMALIZE_AND_CASEFOLD, with the mode by its name, such as NFKC.
template <bool kCasefold>
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): NORMALIZE's text and mode.
bool NormalizeImpl(std::string_view text, std::string_view mode_name, std::string* out,
                   absl::Status* error) {
  googlesql::functions::NormalizeMode mode{};
  if (!googlesql::functions::NormalizeMode_Parse(mode_name, &mode)) {
    *error = absl::OutOfRangeError("Invalid normalize mode");
    return false;
  }
  return googlesql::functions::Normalize(text, mode, kCasefold, out, error);
}

}  // namespace

// RFC 4648's base32, which BigQuery's TO_BASE32 and FROM_BASE32 implement and GoogleSQL leaves
// out of its open source.
constexpr std::string_view kBase32Alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";

std::string ToBase32(std::string_view bytes) {
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
}

// Accepts either case, and text either padded to groups of 8 characters or not padded at all.
absl::StatusOr<std::string> FromBase32(std::string_view text) {
  const std::string_view data = text.substr(0, text.find_last_not_of('=') + 1);
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
}

// CHR, which BigQuery makes an empty string for 0 where GoogleSQL makes NUL.
bool Chr(int64_t codepoint, std::string* out, absl::Status* error) {
  if (codepoint == 0) {
    out->clear();
    return true;
  }
  return googlesql::functions::CodePointToString(codepoint, out, error);
}

// The text of a DECIMAL, which DuckDB pads with zeros to its scale, as BigQuery writes a NUMERIC
// or BIGNUMERIC, without trailing fractional zeros. Every DECIMAL fits a BIGNUMERIC.
absl::StatusOr<std::string> DecimalString(std::string_view text) {
  GOOGLESQL_ASSIGN_OR_RETURN(const auto value, googlesql::BigNumericValue::FromString(text));
  return value.ToString();
}

// The text of a FLOAT64 as BigQuery writes it, the shortest that reads back as the same value,
// without DuckDB's ".0" for a whole number, preserving the sign of negative zero.
bool DoubleString(double value, std::string* out, absl::Status* error) {
  return googlesql::functions::NumericToString(value, out, error, /*canonicalize_zero=*/false);
}

absl::StatusOr<bool> CheckStringLength(std::string_view text, int64_t maximum,
                                       std::string_view column) {
  return CheckLength<false>(text, maximum, column);
}

absl::StatusOr<bool> CheckBytesLength(std::string_view text, int64_t maximum,
                                      std::string_view column) {
  return CheckLength<true>(text, maximum, column);
}

bool Normalize(std::string_view text, std::string_view mode, std::string* out,
               absl::Status* error) {
  return NormalizeImpl<false>(text, mode, out, error);
}

bool NormalizeAndCasefold(std::string_view text, std::string_view mode, std::string* out,
                          absl::Status* error) {
  return NormalizeImpl<true>(text, mode, out, error);
}

}  // namespace googlesql_functions
