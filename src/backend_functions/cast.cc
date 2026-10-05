#include <cstdint>
#include <optional>
#include <string>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/time/time.h"
#include "duckdb.h"
#include "googlesql/public/civil_time.h"
#include "googlesql/public/functions/convert_string.h"
#include "googlesql/public/functions/date_time_util.h"
#include "src/backend_functions/internal.h"

// Casts from STRING that DuckDB accepts in more formats than BigQuery, and casts to STRING that
// DuckDB formats differently. A cast's last argument is true under SAFE_CAST, which makes its
// errors NULL.

namespace bigquery_emulator_duckdb::backend_functions {
namespace {

namespace fn = googlesql::functions;

// CAST(string AS DATE).
void StringToDate(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  EachRow(info, input, output, [](const Arguments& arguments) {
    int32_t out = 0;
    const absl::Status status = fn::ConvertStringToDate(arguments.String(0), &out);
    return OrNull(status.ok(), out, status, arguments.Bool(1));
  });
}

// CAST(string AS TIME).
void StringToTime(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  EachRow(info, input, output, [](const Arguments& arguments) {
    googlesql::TimeValue out;
    const absl::Status status =
        fn::ConvertStringToTime(arguments.String(0), fn::kMicroseconds, &out);
    return OrNull(status.ok(), MicrosFromTime(out), status, arguments.Bool(1));
  });
}

// CAST(string AS DATETIME).
void StringToDatetime(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  EachRow(info, input, output,
          [](const Arguments& arguments) -> absl::StatusOr<std::optional<int64_t>> {
            googlesql::DatetimeValue datetime;
            const absl::Status status =
                fn::ConvertStringToDatetime(arguments.String(0), fn::kMicroseconds, &datetime);
            const auto micros =
                status.ok() ? MicrosFromDatetime(datetime) : absl::StatusOr<int64_t>(status);
            return OrNull(micros.ok(), micros.value_or(0), micros.status(), arguments.Bool(1));
          });
}

// CAST(string AS TIMESTAMP), in UTC unless the string has a time zone.
void StringToTimestamp(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  EachRow(info, input, output, [](const Arguments& arguments) {
    int64_t out = 0;
    const absl::Status status =
        fn::ConvertStringToTimestamp(arguments.String(0), absl::UTCTimeZone(), fn::kMicroseconds,
                                     /*allow_tz_in_str=*/true, &out);
    return OrNull(status.ok(), out, status, arguments.Bool(1));
  });
}

// CAST(float64 AS STRING), which DuckDB writes with a fraction, as "0.0".
void DoubleToString(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  EachRow(info, input, output, [](const Arguments& arguments) {
    std::string out;
    absl::Status error;
    const bool ok =
        fn::NumericToString(arguments.Double(0), &out, &error, /*canonicalize_zero=*/true);
    return ToStatusOr(ok, out, error);
  });
}

// CAST(time AS STRING). DuckDB leaves out trailing zeros of the fraction, where BigQuery writes
// three or six digits.
void TimeToString(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  EachRow(info, input, output, [](const Arguments& arguments) {
    std::string out;
    const absl::Status status =
        fn::ConvertTimeToString(TimeFromMicros(arguments.Int(0)), fn::kMicroseconds, &out);
    return ToStatusOr(status.ok(), out, status);
  });
}

// CAST(datetime AS STRING).
void DatetimeToString(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  EachRow(info, input, output, [](const Arguments& arguments) -> absl::StatusOr<std::string> {
    const auto datetime = DatetimeFromMicros(arguments.Int(0));
    if (!datetime.ok()) {
      return datetime.status();
    }
    std::string out;
    const absl::Status status = fn::ConvertDatetimeToString(*datetime, fn::kMicroseconds, &out);
    return ToStatusOr(status.ok(), out, status);
  });
}

// CAST(timestamp AS STRING), in UTC.
void TimestampToString(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  EachRow(info, input, output, [](const Arguments& arguments) {
    std::string out;
    const absl::Status status = fn::ConvertTimestampToString(
        absl::FromUnixMicros(arguments.Int(0)), fn::kMicroseconds, absl::UTCTimeZone(), &out);
    return ToStatusOr(status.ok(), out, status);
  });
}

}  // namespace

void RegisterCastFunctions(duckdb_connection connection) {
  Register(connection, "bq_cast_date", {kVarchar, kBoolean}, kDate, StringToDate);
  Register(connection, "bq_cast_time", {kVarchar, kBoolean}, kTime, StringToTime);
  Register(connection, "bq_cast_datetime", {kVarchar, kBoolean}, kDatetime, StringToDatetime);
  Register(connection, "bq_cast_timestamp", {kVarchar, kBoolean}, kTimestamp, StringToTimestamp);
  Register(connection, "bq_double_string", {kDouble}, kVarchar, DoubleToString);
  Register(connection, "bq_time_string", {kTime}, kVarchar, TimeToString);
  Register(connection, "bq_datetime_string", {kDatetime}, kVarchar, DatetimeToString);
  Register(connection, "bq_timestamp_string", {kTimestamp}, kVarchar, TimestampToString);
}

}  // namespace bigquery_emulator_duckdb::backend_functions
