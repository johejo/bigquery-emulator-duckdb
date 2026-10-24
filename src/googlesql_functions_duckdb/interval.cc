#include "src/googlesql_functions_duckdb/interval.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/time/time.h"
#include "duckdb.h"
#include "googlesql/base/status_macros.h"
#include "googlesql/public/civil_time.h"
#include "googlesql/public/functions/date_time_util.h"
#include "googlesql/public/functions/datetime.pb.h"
#include "googlesql/public/interval_value.h"
#include "src/duckdb_functions/apply.h"
#include "src/duckdb_functions/register.h"
#include "src/duckdb_functions/scalar.h"
#include "src/duckdb_handle.h"
#include "src/googlesql_functions/datetime.h"
#include "src/interval.h"

namespace bigquery_emulator_duckdb::functions_duckdb {
using namespace duckdb_functions;
using namespace googlesql_functions;
namespace {

namespace fn = googlesql::functions;
using googlesql::IntervalValue;

constexpr duckdb_type kInterval = DUCKDB_TYPE_INTERVAL;

absl::StatusOr<IntervalValue> MakeInterval(int64_t years, int64_t months, int64_t days,
                                           int64_t hours, int64_t minutes, int64_t seconds) {
  auto value = IntervalValue::FromYMDHMS(years, months, days, hours, minutes, seconds);
  if (!value.ok()) {
    // BigQuery names the function after GoogleSQL's message.
    return absl::Status(value.status().code(), absl::StrCat(value.status().message(),
                                                            "; error in MAKE_INTERVAL expression"));
  }
  return value;
}

absl::StatusOr<IntervalValue> IntervalConstructor(int64_t value, std::string_view part_name) {
  fn::DateTimestampPart part = fn::DATE;
  if (!fn::DateTimestampPart_Parse(part_name, &part)) {
    return absl::InvalidArgumentError("Unsupported INTERVAL date part");
  }
  return IntervalValue::FromInteger(value, part, false);
}

// The + and - operators over INTERVAL values. GoogleSQL adds the months, then the days, then the
// time, and fails when a step leaves the range, so these add them through GoogleSQL rather than
// with DuckDB's + and -, which check only the result. Like GoogleSQL, - adds the negated INTERVAL.
template <bool kSubtract>
IntervalValue Operand(const IntervalValue& interval) {
  return kSubtract ? -interval : interval;
}

template <bool kSubtract>
absl::StatusOr<IntervalValue> AddIntervals(const IntervalValue& left, const IntervalValue& right) {
  return left + Operand<kSubtract>(right);
}

// INTERVAL * INT64, and INT64 * INTERVAL. GoogleSQL multiplies the months, days and time apart, and
// fails where one leaves the range.
absl::StatusOr<IntervalValue> MultiplyInterval(const IntervalValue& interval, int64_t factor) {
  return interval * factor;
}

// INTERVAL / INT64. GoogleSQL divides the months, carries the remainder into the days and theirs
// into the time, and truncates the time to microseconds.
absl::StatusOr<IntervalValue> DivideInterval(const IntervalValue& interval, int64_t divisor) {
  return interval.Divide(divisor, /*round_to_micros=*/true);
}

// DATE + INTERVAL, which is a DATETIME.
template <bool kSubtract>
absl::StatusOr<int64_t> AddDateInterval(int32_t date, const IntervalValue& interval) {
  googlesql::DatetimeValue result;
  GOOGLESQL_RETURN_IF_ERROR(fn::AddDate(date, Operand<kSubtract>(interval), &result));
  return MicrosFromDatetime(result);
}

template <bool kSubtract>
absl::StatusOr<int64_t> AddDatetimeInterval(int64_t micros, const IntervalValue& interval) {
  GOOGLESQL_ASSIGN_OR_RETURN(const auto datetime, DatetimeFromMicros(micros));
  googlesql::DatetimeValue result;
  GOOGLESQL_RETURN_IF_ERROR(fn::AddDatetime(datetime, Operand<kSubtract>(interval), &result));
  return MicrosFromDatetime(result);
}

// GoogleSQL rejects intervals with months, and names the time zone only in its errors, where
// BigQuery's default is UTC.
template <bool kSubtract>
absl::StatusOr<int64_t> AddTimestampInterval(int64_t micros, const IntervalValue& interval) {
  absl::Time result;
  GOOGLESQL_RETURN_IF_ERROR(fn::AddTimestamp(absl::FromUnixMicros(micros), absl::UTCTimeZone(),
                                             Operand<kSubtract>(interval), &result));
  return absl::ToUnixMicros(result);
}

// DATE - DATE, DATETIME - DATETIME, TIMESTAMP - TIMESTAMP and TIME - TIME. GoogleSQL gives the
// difference of dates in days, of datetimes in days and time, and of the others in time.
absl::StatusOr<IntervalValue> SubtractDatetimes(int64_t left, int64_t right) {
  GOOGLESQL_ASSIGN_OR_RETURN(const auto left_datetime, DatetimeFromMicros(left));
  GOOGLESQL_ASSIGN_OR_RETURN(const auto right_datetime, DatetimeFromMicros(right));
  return fn::IntervalDiffDatetimes(left_datetime, right_datetime);
}

absl::StatusOr<IntervalValue> SubtractTimes(int64_t left, int64_t right) {
  return fn::IntervalDiffTimes(TimeFromMicros(left), TimeFromMicros(right));
}

absl::StatusOr<IntervalValue> SubtractTimestamps(int64_t left, int64_t right) {
  return fn::IntervalDiffTimestamps(absl::FromUnixMicros(left), absl::FromUnixMicros(right));
}

// SUM, or with `kAverage` AVG, of the non-NULL INTERVAL values the translator collects with list().
// GoogleSQL sums months, days and time apart and fails only when the result is out of range.
// BigQuery keeps microseconds, so the average truncates the nanoseconds the division leaves.
// With `kDistinct`, equal values count once, as the first of them in input order, as BigQuery
// counts them.
template <bool kAverage, bool kDistinct>
void IntervalAggregate(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  EachRow(info, input, output,
          [](const Arguments& args) -> absl::StatusOr<std::optional<duckdb_interval>> {
            duckdb_vector vector = args.Vector(0);
            const auto entry = VectorElement<duckdb_list_entry>(vector, args.Row());
            if (entry.length == 0) {
              return std::nullopt;
            }
            duckdb_vector child = duckdb_list_vector_get_child(vector);
            googlesql::IntervalValue::SumAggregator sum;
            std::unordered_set<int64_t> lengths;
            int64_t count = 0;
            for (idx_t i = 0; i < entry.length; ++i) {
              GOOGLESQL_ASSIGN_OR_RETURN(
                  const auto value,
                  IntervalFromDuckDb(VectorElement<duckdb_interval>(child, entry.offset + i)));
              if (kDistinct && !lengths.insert(value.GetAsMicros()).second) {
                continue;
              }
              sum.Add(value);
              ++count;
            }
            GOOGLESQL_ASSIGN_OR_RETURN(
                const auto result,
                kAverage ? sum.GetAverage(count, /*round_to_micros=*/true) : sum.GetSum());
            return IntervalToDuckDb(result);
          });
}

// ARRAY_AGG(DISTINCT ...) of the INTERVAL values the translator collects with list(): equal values,
// and NULL values, occur once, as the first of them in input order, as BigQuery keeps them.
void IntervalDistinct(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  EachRow(
      info, input, output,
      [](const Arguments& args) -> absl::StatusOr<std::vector<std::optional<duckdb_interval>>> {
        duckdb_vector vector = args.Vector(0);
        const auto entry = VectorElement<duckdb_list_entry>(vector, args.Row());
        duckdb_vector child = duckdb_list_vector_get_child(vector);
        uint64_t* validity = duckdb_vector_get_validity(child);
        std::vector<std::optional<duckdb_interval>> values;
        std::unordered_set<int64_t> lengths;
        bool null = false;
        for (idx_t i = 0; i < entry.length; ++i) {
          if (validity != nullptr && !duckdb_validity_row_is_valid(validity, entry.offset + i)) {
            if (!std::exchange(null, true)) {
              values.emplace_back();
            }
            continue;
          }
          const auto element = VectorElement<duckdb_interval>(child, entry.offset + i);
          GOOGLESQL_ASSIGN_OR_RETURN(const auto value, IntervalFromDuckDb(element));
          if (lengths.insert(value.GetAsMicros()).second) {
            values.emplace_back(element);
          }
        }
        return values;
      });
}

// CAST from STRING, which takes the canonical form or ISO 8601.
absl::StatusOr<IntervalValue> IntervalFromString(std::string_view text) {
  return IntervalValue::Parse(text, /*allow_nanos=*/false);
}

// A CSV value, which BigQuery documents only as Y-M D H:M:S[.F].
absl::StatusOr<IntervalValue> IntervalFromCsv(std::string_view text) {
  return IntervalValue::ParseFromString(text, fn::YEAR, fn::SECOND, /*allow_nanos=*/false);
}

std::string IntervalString(const IntervalValue& value) { return value.ToString(); }

// GoogleSQL compares, and so orders, INTERVAL values by their length with 30 days to a month and
// 24 hours to a day, while DuckDB compares the components it carries the remainders into.
int64_t IntervalKey(const IntervalValue& value) { return value.GetAsMicros(); }

}  // namespace

void RegisterIntervalFunctions(duckdb_connection connection) {
  Register(connection, "bq_make_interval", {kBigint, kBigint, kBigint, kBigint, kBigint, kBigint},
           kInterval, Apply<MakeInterval>);
  Register(connection, "bq_interval_parts", {kBigint, kBigint, kBigint}, kInterval,
           Apply<IntervalValue::FromMonthsDaysMicros>);
  Register(connection, "bq_interval", {kBigint, kVarchar}, kInterval, Apply<IntervalConstructor>);
  Register(connection, "bq_justify_hours", {kInterval}, kInterval, Apply<googlesql::JustifyHours>);
  Register(connection, "bq_justify_days", {kInterval}, kInterval, Apply<googlesql::JustifyDays>);
  Register(connection, "bq_justify_interval", {kInterval}, kInterval,
           Apply<googlesql::JustifyInterval>);
  const LogicalType interval(duckdb_create_logical_type(kInterval));
  const LogicalType list(duckdb_create_list_type(interval.get()));
  Register(connection, "bq_interval_sum", {list.get()}, interval.get(),
           IntervalAggregate<false, false>);
  Register(connection, "bq_interval_avg", {list.get()}, interval.get(),
           IntervalAggregate<true, false>);
  Register(connection, "bq_interval_sum_distinct", {list.get()}, interval.get(),
           IntervalAggregate<false, true>);
  Register(connection, "bq_interval_avg_distinct", {list.get()}, interval.get(),
           IntervalAggregate<true, true>);
  Register(connection, "bq_interval_distinct", {list.get()}, list.get(), IntervalDistinct);
  Register(connection, "bq_interval_string", {kInterval}, kVarchar, Apply<IntervalString>);
  Register(connection, "bq_interval_from_string", {kVarchar, kBoolean}, kInterval,
           Apply<IntervalFromString, Errors::kNullUnderSafeCast>);
  Register(connection, "bq_interval_from_csv", {kVarchar}, kInterval, Apply<IntervalFromCsv>);
  Register(connection, "bq_interval_key", {kInterval}, kBigint, Apply<IntervalKey>);
  Register(connection, "bq_add_intervals", {kInterval, kInterval}, kInterval,
           Apply<AddIntervals<false>>);
  Register(connection, "bq_subtract_intervals", {kInterval, kInterval}, kInterval,
           Apply<AddIntervals<true>>);
  Register(connection, "bq_multiply_interval", {kInterval, kBigint}, kInterval,
           Apply<MultiplyInterval>);
  Register(connection, "bq_divide_interval", {kInterval, kBigint}, kInterval,
           Apply<DivideInterval>);
  Register(connection, "bq_add_date_interval", {kDate, kInterval}, kDatetime,
           Apply<AddDateInterval<false>>);
  Register(connection, "bq_subtract_date_interval", {kDate, kInterval}, kDatetime,
           Apply<AddDateInterval<true>>);
  Register(connection, "bq_add_datetime_interval", {kDatetime, kInterval}, kDatetime,
           Apply<AddDatetimeInterval<false>>);
  Register(connection, "bq_subtract_datetime_interval", {kDatetime, kInterval}, kDatetime,
           Apply<AddDatetimeInterval<true>>);
  Register(connection, "bq_add_timestamp_interval", {kTimestamp, kInterval}, kTimestamp,
           Apply<AddTimestampInterval<false>>);
  Register(connection, "bq_subtract_timestamp_interval", {kTimestamp, kInterval}, kTimestamp,
           Apply<AddTimestampInterval<true>>);
  Register(connection, "bq_subtract_dates", {kDate, kDate}, kInterval,
           Apply<fn::IntervalDiffDates>);
  Register(connection, "bq_subtract_datetimes", {kDatetime, kDatetime}, kInterval,
           Apply<SubtractDatetimes>);
  Register(connection, "bq_subtract_times", {kTime, kTime}, kInterval, Apply<SubtractTimes>);
  Register(connection, "bq_subtract_timestamps", {kTimestamp, kTimestamp}, kInterval,
           Apply<SubtractTimestamps>);
}

}  // namespace bigquery_emulator_duckdb::functions_duckdb
