#include "src/interval.h"

#include <cstdint>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "duckdb.h"

namespace bigquery_emulator_duckdb {
namespace {

using googlesql::IntervalValue;

// Retain the native representation for microsecond values, including persisted rows. For other
// values, use disjoint bands in the months field to encode the 0..999 nanosecond remainder.
// The band width also exceeds the full range of interval lengths measured in 30-day months,
// so different remainders cannot compare equal even in DuckDB's internal comparisons.
constexpr int64_t kMonthStride = 1'000'000;
constexpr int64_t kMaxLengthMicros = (IntervalValue::kMaxMonths * IntervalValue::kMicrosInMonth) +
                                     (IntervalValue::kMaxDays * IntervalValue::kMicrosInDay) +
                                     IntervalValue::kMaxMicros;
static_assert(kMaxLengthMicros * 2 < kMonthStride * IntervalValue::kMicrosInMonth);
static_assert((kMonthStride * (IntervalValue::kNanosInMicro - 1)) + IntervalValue::kMaxMonths <=
              INT32_MAX);

}  // namespace

absl::StatusOr<IntervalValue> IntervalFromDuckDb(const duckdb_interval& value) {
  const int64_t fraction = (value.months + (kMonthStride / 2)) / kMonthStride;
  if (fraction < 0 || fraction >= IntervalValue::kNanosInMicro) {
    return absl::OutOfRangeError("Invalid interval nanosecond remainder");
  }
  return IntervalValue::FromMonthsDaysNanos(
      value.months - (fraction * kMonthStride), value.days,
      (value.micros * IntervalValue::kNanosInMicro128) + fraction);
}

duckdb_interval IntervalToDuckDb(const googlesql::IntervalValue& value) {
  return {
      .months =
          static_cast<int32_t>(value.get_months() + (value.get_nano_fractions() * kMonthStride)),
      .days = static_cast<int32_t>(value.get_days()),
      .micros = value.get_micros(),
  };
}

}  // namespace bigquery_emulator_duckdb
