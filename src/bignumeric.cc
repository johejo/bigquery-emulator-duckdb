#include "src/bignumeric.h"

#include <cstddef>
#include <string>
#include <string_view>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "googlesql/public/numeric_value.h"

namespace bigquery_emulator_duckdb {
namespace {

constexpr size_t kScale = googlesql::BigNumericValue::kMaxFractionalDigits;
// A BIGNUMERIC is a 256-bit integer of units.
constexpr size_t kBytes = 32;

}  // namespace

std::string BigNumericUnits(const googlesql::BigNumericValue& value) {
  // ToString() writes the digits without an exponent or trailing fractional zeros.
  std::string text = value.ToString();
  const bool negative = text.starts_with('-');
  if (negative) {
    text.erase(0, 1);
  }
  const size_t point = text.find('.');
  std::string fraction = point == std::string::npos ? "" : text.substr(point + 1);
  fraction.append(kScale - fraction.size(), '0');
  std::string units = text.substr(0, point) + fraction;
  units.erase(0, units.find_first_not_of('0'));
  if (units.empty()) {
    return "0";
  }
  return negative ? "-" + units : units;
}

absl::StatusOr<googlesql::BigNumericValue> BigNumericFromUnits(std::string_view units) {
  std::string digits(units);
  const bool negative = digits.starts_with('-');
  if (negative) {
    digits.erase(0, 1);
  }
  if (digits.empty() || digits.find_first_not_of("0123456789") != std::string::npos) {
    return absl::InvalidArgumentError("Invalid BIGNUMERIC units: " + std::string(units));
  }
  if (digits.size() <= kScale) {
    digits.insert(0, kScale + 1 - digits.size(), '0');
  }
  digits.insert(digits.size() - kScale, ".");
  return googlesql::BigNumericValue::FromStringStrict((negative ? "-" : "") + digits);
}

absl::StatusOr<googlesql::BigNumericValue> BigNumericFromBignum(std::string_view stored) {
  // A three byte header, whose top bit is set for a value that is not negative, then the big
  // endian bytes of its absolute value, with every bit inverted for a negative value. GoogleSQL
  // reads the little endian two's complement of the units.
  if (stored.size() < 4) {
    return absl::InvalidArgumentError("Invalid BIGNUM");
  }
  const bool negative = (static_cast<unsigned char>(stored[0]) & 0x80) == 0;
  std::string bytes(stored.rbegin(), stored.rend() - 3);
  if (negative) {
    // The bytes hold the absolute value inverted, which plus one is its negation.
    for (char& byte : bytes) {
      byte = static_cast<char>(byte + 1);
      if (byte != 0) {
        break;
      }
    }
  }
  if (((static_cast<unsigned char>(bytes.back()) & 0x80) != 0) != negative) {
    bytes.push_back(negative ? '\xff' : '\0');
  }
  if (bytes.size() > kBytes) {
    return absl::OutOfRangeError("BIGNUMERIC overflow");
  }
  return googlesql::BigNumericValue::DeserializeFromProtoBytes(bytes);
}

std::string BigNumericSql(const googlesql::BigNumericValue& value) {
  return "CAST('" + BigNumericUnits(value) + "' AS BIGNUM)";
}

}  // namespace bigquery_emulator_duckdb
