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

std::string BigNumericSql(const googlesql::BigNumericValue& value) {
  return "CAST('" + BigNumericUnits(value) + "' AS BIGNUM)";
}

}  // namespace bigquery_emulator_duckdb
