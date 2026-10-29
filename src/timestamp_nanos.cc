#include "src/timestamp_nanos.h"

#include <bit>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/time/time.h"
#include "duckdb.h"
#include "googlesql/base/status_macros.h"
#include "googlesql/public/functions/date_time_util.h"
#include "src/duckdb_sql.h"
#include "src/timestamp_precision.h"

namespace bigquery_emulator_duckdb {
namespace {

constexpr int64_t kMinSeconds = -62135596800;
constexpr int64_t kEndSeconds = 253402300800;
constexpr int64_t kNanosPerSecond = 1000000000;

}  // namespace

absl::StatusOr<__int128> TimestampNanos(absl::Time time) {
  if (time < absl::FromUnixSeconds(kMinSeconds) || time >= absl::FromUnixSeconds(kEndSeconds)) {
    return absl::OutOfRangeError("TIMESTAMP is outside years 0001 through 9999");
  }
  const int64_t seconds = absl::ToUnixSeconds(time);
  const absl::Duration fraction = time - absl::FromUnixSeconds(seconds);
  const int64_t nanos = absl::ToInt64Nanoseconds(fraction);
  if (fraction != absl::Nanoseconds(nanos)) {
    return absl::OutOfRangeError("Subnanosecond TIMESTAMP precision is unsupported");
  }
  return (static_cast<__int128>(seconds) * kNanosPerSecond) + nanos;
}

absl::StatusOr<absl::Time> TimestampFromNanos(__int128 nanos) {
  if (nanos < static_cast<__int128>(kMinSeconds) * kNanosPerSecond ||
      nanos >= static_cast<__int128>(kEndSeconds) * kNanosPerSecond) {
    return absl::OutOfRangeError("TIMESTAMP is outside years 0001 through 9999");
  }
  return absl::FromUnixSeconds(static_cast<int64_t>(nanos / kNanosPerSecond)) +
         absl::Nanoseconds(static_cast<int64_t>(nanos % kNanosPerSecond));
}

absl::StatusOr<__int128> ParseTimestampNanos(std::string_view text) {
  absl::Time time;
  GOOGLESQL_RETURN_IF_ERROR(googlesql::functions::ConvertStringToTimestamp(
      text, absl::UTCTimeZone(), googlesql::functions::kNanoseconds,
      /*allow_tz_in_str=*/true, &time));
  return TimestampNanos(time);
}

absl::StatusOr<std::string> TimestampNanosText(__int128 nanos) {
  GOOGLESQL_ASSIGN_OR_RETURN(const absl::Time time, TimestampFromNanos(nanos));
  std::string text;
  GOOGLESQL_RETURN_IF_ERROR(googlesql::functions::ConvertTimestampToString(
      time, googlesql::functions::kNanoseconds, "UTC", &text));
  return text;
}

__int128 TimestampNanosInteger(duckdb_hugeint value) {
  return std::bit_cast<__int128>((static_cast<unsigned __int128>(value.upper) << 64U) |
                                 value.lower);
}

std::string TimestampNanosSql(absl::Time time) {
  const auto nanos = TimestampNanos(time);
  if (!nanos.ok()) {
    throw std::invalid_argument(std::string(nanos.status().message()));
  }
  // Spell seconds and the remainder separately: neither needs a nonstandard integer formatter.
  const std::string seconds = std::to_string(static_cast<int64_t>(*nanos / kNanosPerSecond));
  const std::string fraction = std::to_string(static_cast<int64_t>(*nanos % kNanosPerSecond));
  return "CAST((CAST(" + QuoteLiteral(seconds) + " AS HUGEINT) * 1000000000 + " + fraction +
         ") AS " + kTimestampNanosType + ")";
}

}  // namespace bigquery_emulator_duckdb
