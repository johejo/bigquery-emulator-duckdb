#include "src/bignumeric.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>

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
  const bool negative = (static_cast<unsigned char>(stored.at(0)) & 0x80U) == 0;
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
  if (((static_cast<unsigned char>(bytes.back()) & 0x80U) != 0) != negative) {
    bytes.push_back(negative ? '\xff' : '\0');
  }
  if (bytes.size() > kBytes) {
    return absl::OutOfRangeError("BIGNUMERIC overflow");
  }
  return googlesql::BigNumericValue::DeserializeFromProtoBytes(bytes);
}

std::string BigNumericBignum(const googlesql::BigNumericValue& value) {
  // The little endian two's complement of the units, made their absolute value.
  std::string bytes = value.SerializeAsProtoBytes();
  const bool negative = (static_cast<unsigned char>(bytes.back()) & 0x80U) != 0;
  if (negative) {
    // Extended by a byte of the sign, so that negating the most negative value cannot overflow,
    // then inverted and incremented.
    bytes.push_back(static_cast<char>(0xff));
    std::ranges::transform(bytes, bytes.begin(), [](char byte) {
      return static_cast<char>(~static_cast<unsigned char>(byte));
    });
    for (char& byte : bytes) {
      byte = static_cast<char>(byte + 1);
      if (byte != 0) {
        break;
      }
    }
  }
  while (bytes.size() > 1 && bytes.back() == 0) {
    bytes.pop_back();
  }
  // The header holds the number of bytes, with its top bit set, all inverted for a negative
  // value, as are the big endian bytes of the absolute value that follow it.
  uint32_t header = static_cast<uint32_t>(bytes.size()) | 0x800000U;
  if (negative) {
    header = ~header;
  }
  std::string stored = {
      static_cast<char>(header >> 16U),
      static_cast<char>(header >> 8U),
      static_cast<char>(header),
  };
  for (const char byte : std::views::reverse(bytes)) {
    stored += negative ? static_cast<char>(~static_cast<unsigned char>(byte)) : byte;
  }
  return stored;
}

std::string BigNumericTypeName(int64_t precision, int64_t scale) {
  return "bq_bignumeric_" + std::to_string(precision) + "_" + std::to_string(scale);
}

absl::StatusOr<googlesql::BigNumericValue> BigNumericFromDecimalBytes(std::string_view bytes,
                                                                      int64_t scale) {
  if (bytes.empty() || scale < 0 || std::cmp_greater(scale, kScale)) {
    return absl::InvalidArgumentError("Invalid Parquet DECIMAL");
  }
  // GoogleSQL reads the little endian two's complement of an integer, without the bytes that
  // only extend the sign. The integer then reads as units, whose digits give the decimal text.
  std::string little(bytes.rbegin(), bytes.rend());
  const auto sign = [](char byte) { return (static_cast<unsigned char>(byte) & 0x80U) != 0; };
  while (little.size() > 1 && little.back() == (sign(little.back()) ? '\xff' : '\0') &&
         sign(little.at(little.size() - 2)) == sign(little.back())) {
    little.pop_back();
  }
  if (little.size() > kBytes) {
    return absl::OutOfRangeError("BIGNUMERIC overflow");
  }
  auto integer = googlesql::BigNumericValue::DeserializeFromProtoBytes(little);
  if (!integer.ok() || std::cmp_equal(scale, kScale)) {
    return integer;
  }
  std::string digits = BigNumericUnits(*integer);
  const bool negative = digits.starts_with('-');
  if (negative) {
    digits.erase(0, 1);
  }
  const auto point = static_cast<size_t>(scale);
  if (digits.size() <= point) {
    digits.insert(0, point + 1 - digits.size(), '0');
  }
  digits.insert(digits.size() - point, ".");
  return googlesql::BigNumericValue::FromStringStrict((negative ? "-" : "") + digits);
}

std::string BigNumericDecimalBytes(const googlesql::BigNumericValue& value) {
  std::string little = value.SerializeAsProtoBytes();
  little.resize(kBytes, (static_cast<unsigned char>(little.back()) & 0x80U) != 0 ? '\xff' : '\0');
  return {little.rbegin(), little.rend()};
}

std::string BigNumericSql(const googlesql::BigNumericValue& value) {
  return "CAST('" + BigNumericUnits(value) + "' AS BIGNUM)";
}

}  // namespace bigquery_emulator_duckdb
