#include "src/backend_functions/interval.h"

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_set>

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
#include "src/backend_functions/datetime.h"
#include "src/backend_functions/register.h"
#include "src/backend_functions/scalar.h"
#include "src/duckdb_handle.h"
#include "src/interval.h"

namespace bigquery_emulator_duckdb::backend_functions {
namespace {

constexpr duckdb_type kInterval = DUCKDB_TYPE_INTERVAL;

absl::StatusOr<googlesql::IntervalValue> ReadInterval(const Arguments& args, idx_t column = 0) {
  return IntervalFromDuckDb(VectorElement<duckdb_interval>(args.Vector(column), args.Row()));
}

void MakeInterval(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  EachRow(info, input, output, [](const Arguments& args) -> absl::StatusOr<duckdb_interval> {
    auto value = googlesql::IntervalValue::FromYMDHMS(args.Int(0), args.Int(1), args.Int(2),
                                                      args.Int(3), args.Int(4), args.Int(5));
    if (!value.ok()) {
      // BigQuery names the function after GoogleSQL's message.
      return absl::Status(
          value.status().code(),
          absl::StrCat(value.status().message(), "; error in MAKE_INTERVAL expression"));
    }
    return IntervalToDuckDb(*value);
  });
}

// Resolved literals already carry validated components; this also validates the physical value.
void IntervalParts(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  EachRow(info, input, output, [](const Arguments& args) -> absl::StatusOr<duckdb_interval> {
    GOOGLESQL_ASSIGN_OR_RETURN(const auto value, googlesql::IntervalValue::FromMonthsDaysMicros(
                                                     args.Int(0), args.Int(1), args.Int(2)));
    return IntervalToDuckDb(value);
  });
}

void IntervalConstructor(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  EachRow(info, input, output, [](const Arguments& args) -> absl::StatusOr<duckdb_interval> {
    googlesql::functions::DateTimestampPart part = googlesql::functions::DATE;
    if (!googlesql::functions::DateTimestampPart_Parse(args.String(1), &part)) {
      return absl::InvalidArgumentError("Unsupported INTERVAL date part");
    }
    GOOGLESQL_ASSIGN_OR_RETURN(const auto value,
                               googlesql::IntervalValue::FromInteger(args.Int(0), part, false));
    return IntervalToDuckDb(value);
  });
}

template <auto Justify>
void JustifyInterval(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  EachRow(info, input, output, [](const Arguments& args) -> absl::StatusOr<duckdb_interval> {
    GOOGLESQL_ASSIGN_OR_RETURN(const auto value, ReadInterval(args));
    GOOGLESQL_ASSIGN_OR_RETURN(const auto result, Justify(value));
    return IntervalToDuckDb(result);
  });
}

// The + operator over INTERVAL values. GoogleSQL adds the months, then the days, then the time,
// and fails when a step leaves the range, so these add them through GoogleSQL rather than with
// DuckDB's +, which checks only the result.
void AddIntervals(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  EachRow(info, input, output, [](const Arguments& args) -> absl::StatusOr<duckdb_interval> {
    GOOGLESQL_ASSIGN_OR_RETURN(const auto left, ReadInterval(args));
    GOOGLESQL_ASSIGN_OR_RETURN(const auto right, ReadInterval(args, 1));
    GOOGLESQL_ASSIGN_OR_RETURN(const auto result, left + right);
    return IntervalToDuckDb(result);
  });
}

// DATE + INTERVAL, which is a DATETIME.
void AddDateInterval(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  EachRow(info, input, output, [](const Arguments& args) -> absl::StatusOr<int64_t> {
    GOOGLESQL_ASSIGN_OR_RETURN(const auto interval, ReadInterval(args, 1));
    googlesql::DatetimeValue result;
    GOOGLESQL_RETURN_IF_ERROR(googlesql::functions::AddDate(args.Date(0), interval, &result));
    return MicrosFromDatetime(result);
  });
}

void AddDatetimeInterval(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  EachRow(info, input, output, [](const Arguments& args) -> absl::StatusOr<int64_t> {
    GOOGLESQL_ASSIGN_OR_RETURN(const auto datetime, DatetimeFromMicros(args.Int(0)));
    GOOGLESQL_ASSIGN_OR_RETURN(const auto interval, ReadInterval(args, 1));
    googlesql::DatetimeValue result;
    GOOGLESQL_RETURN_IF_ERROR(googlesql::functions::AddDatetime(datetime, interval, &result));
    return MicrosFromDatetime(result);
  });
}

// GoogleSQL rejects intervals with months, and names the time zone only in its errors, where
// BigQuery's default is UTC.
void AddTimestampInterval(duckdb_function_info info, duckdb_data_chunk input,
                          duckdb_vector output) {
  EachRow(info, input, output, [](const Arguments& args) -> absl::StatusOr<int64_t> {
    GOOGLESQL_ASSIGN_OR_RETURN(const auto interval, ReadInterval(args, 1));
    absl::Time result;
    GOOGLESQL_RETURN_IF_ERROR(googlesql::functions::AddTimestamp(
        absl::FromUnixMicros(args.Int(0)), absl::UTCTimeZone(), interval, &result));
    return absl::ToUnixMicros(result);
  });
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

// CAST from STRING, which takes the canonical form or ISO 8601, and which SAFE_CAST, when the
// second argument is true, makes NULL on an error.
void IntervalFromString(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  EachRow(info, input, output,
          [](const Arguments& args) -> absl::StatusOr<std::optional<duckdb_interval>> {
            const auto value =
                googlesql::IntervalValue::Parse(args.String(0), /*allow_nanos=*/false);
            if (!value.ok()) {
              if (args.Bool(1)) {
                return std::nullopt;
              }
              return value.status();
            }
            return IntervalToDuckDb(*value);
          });
}

// A CSV value, which BigQuery documents only as Y-M D H:M:S[.F].
void IntervalFromCsv(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  EachRow(info, input, output, [](const Arguments& args) -> absl::StatusOr<duckdb_interval> {
    GOOGLESQL_ASSIGN_OR_RETURN(const auto value, googlesql::IntervalValue::ParseFromString(
                                                     args.String(0), googlesql::functions::YEAR,
                                                     googlesql::functions::SECOND,
                                                     /*allow_nanos=*/false));
    return IntervalToDuckDb(value);
  });
}

void IntervalString(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  EachRow(info, input, output, [](const Arguments& args) -> absl::StatusOr<std::string> {
    GOOGLESQL_ASSIGN_OR_RETURN(const auto value, ReadInterval(args));
    return value.ToString();
  });
}

// GoogleSQL compares, and so orders, INTERVAL values by their length with 30 days to a month and
// 24 hours to a day, while DuckDB compares the components it carries the remainders into.
void IntervalKey(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  EachRow(info, input, output, [](const Arguments& args) -> absl::StatusOr<int64_t> {
    GOOGLESQL_ASSIGN_OR_RETURN(const auto value, ReadInterval(args));
    return value.GetAsMicros();
  });
}

}  // namespace

void RegisterIntervalFunctions(duckdb_connection connection) {
  Register(connection, "bq_make_interval", {kBigint, kBigint, kBigint, kBigint, kBigint, kBigint},
           kInterval, MakeInterval);
  Register(connection, "bq_interval_parts", {kBigint, kBigint, kBigint}, kInterval, IntervalParts);
  Register(connection, "bq_interval", {kBigint, kVarchar}, kInterval, IntervalConstructor);
  Register(connection, "bq_justify_hours", {kInterval}, kInterval,
           JustifyInterval<googlesql::JustifyHours>);
  Register(connection, "bq_justify_days", {kInterval}, kInterval,
           JustifyInterval<googlesql::JustifyDays>);
  Register(connection, "bq_justify_interval", {kInterval}, kInterval,
           JustifyInterval<googlesql::JustifyInterval>);
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
  Register(connection, "bq_interval_string", {kInterval}, kVarchar, IntervalString);
  Register(connection, "bq_interval_from_string", {kVarchar, kBoolean}, kInterval,
           IntervalFromString);
  Register(connection, "bq_interval_from_csv", {kVarchar}, kInterval, IntervalFromCsv);
  Register(connection, "bq_interval_key", {kInterval}, kBigint, IntervalKey);
  Register(connection, "bq_add_intervals", {kInterval, kInterval}, kInterval, AddIntervals);
  Register(connection, "bq_add_date_interval", {kDate, kInterval}, kDatetime, AddDateInterval);
  Register(connection, "bq_add_datetime_interval", {kDatetime, kInterval}, kDatetime,
           AddDatetimeInterval);
  Register(connection, "bq_add_timestamp_interval", {kTimestamp, kInterval}, kTimestamp,
           AddTimestampInterval);
}

}  // namespace bigquery_emulator_duckdb::backend_functions
