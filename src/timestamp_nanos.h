#pragma once

#include <string>
#include <string_view>

#include "absl/status/statusor.h"
#include "absl/time/time.h"
#include "duckdb.h"

namespace bigquery_emulator_duckdb {

// Unix nanoseconds in 128 bits cover years 0001 through 9999, unlike TIMESTAMP_NS. The codec
// rejects out-of-range times and subnanosecond fractions rather than silently losing precision.
absl::StatusOr<__int128> TimestampNanos(absl::Time time);
absl::StatusOr<absl::Time> TimestampFromNanos(__int128 nanos);
absl::StatusOr<__int128> ParseTimestampNanos(std::string_view text);
absl::StatusOr<std::string> TimestampNanosText(__int128 nanos);

__int128 TimestampNanosInteger(duckdb_hugeint value);
std::string TimestampNanosSql(absl::Time time);

}  // namespace bigquery_emulator_duckdb
