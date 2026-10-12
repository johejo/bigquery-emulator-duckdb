#include "src/backend_functions/datetime.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/time/time.h"
#include "duckdb.h"
#include "googlesql/base/status_macros.h"
#include "googlesql/public/civil_time.h"
#include "googlesql/public/functions/date_time_util.h"
#include "googlesql/public/functions/parse_date_time.h"
#include "googlesql/public/functions/string_format.h"
#include "googlesql/public/numeric_value.h"
#include "googlesql/public/options.pb.h"
#include "googlesql/public/value.h"
#include "src/backend_functions/apply.h"
#include "src/backend_functions/register.h"
#include "src/backend_functions/scalar.h"
#include "src/bignumeric.h"
#include "src/duckdb_handle.h"

namespace bigquery_emulator_duckdb::backend_functions {

googlesql::TimeValue TimeFromMicros(int64_t micros) {
  constexpr int64_t kPerSecond = 1000000;
  const int64_t seconds = micros / kPerSecond;
  return googlesql::TimeValue::FromHMSAndMicros(
      static_cast<int32_t>(seconds / 3600), static_cast<int32_t>(seconds / 60 % 60),
      static_cast<int32_t>(seconds % 60), static_cast<int32_t>(micros % kPerSecond));
}

int64_t MicrosFromTime(const googlesql::TimeValue& time) {
  return (((((int64_t{time.Hour()} * 60) + time.Minute()) * 60) + time.Second()) * 1000000) +
         time.Microseconds();
}

absl::StatusOr<googlesql::DatetimeValue> DatetimeFromMicros(int64_t micros) {
  googlesql::DatetimeValue datetime;
  const absl::Status status = googlesql::functions::ConvertTimestampToDatetime(
      absl::FromUnixMicros(micros), absl::UTCTimeZone(), &datetime);
  return ToStatusOr(status.ok(), datetime, status);
}

absl::StatusOr<int64_t> MicrosFromDatetime(const googlesql::DatetimeValue& datetime) {
  absl::Time time;
  const absl::Status status =
      googlesql::functions::ConvertDatetimeToTimestamp(datetime, absl::UTCTimeZone(), &time);
  return ToStatusOr(status.ok(), absl::ToUnixMicros(time), status);
}

namespace {

// The DuckDB type of each argument column.
std::vector<duckdb_type> ColumnTypes(duckdb_data_chunk input) {
  std::vector<duckdb_type> types;
  for (idx_t column = 0; column < duckdb_data_chunk_get_column_count(input); ++column) {
    LogicalType const type(
        duckdb_vector_get_column_type(duckdb_data_chunk_get_vector(input, column)));
    types.push_back(duckdb_get_type_id(type.get()));
  }
  return types;
}

namespace fn = googlesql::functions;

// BigQuery expands %Q to the quarter and %J to the ISO day of the year.
constexpr fn::FormatDateTimestampOptions kFormatOptions = {
    .expand_Q = true,
    .expand_J = true,
};

// FORMAT_DATE(format, date).
absl::Status FormatDate(std::string_view format, int32_t date, std::string* out) {
  return fn::FormatDateToString(format, date, kFormatOptions, out);
}

// FORMAT_DATETIME(format, datetime).
absl::Status FormatDatetime(std::string_view format, int64_t micros, std::string* out) {
  GOOGLESQL_ASSIGN_OR_RETURN(const auto datetime, DatetimeFromMicros(micros));
  return fn::FormatDatetimeToStringWithOptions(format, datetime, kFormatOptions, out);
}

// FORMAT_TIME(format, time).
absl::Status FormatTime(std::string_view format, int64_t micros, std::string* out) {
  return fn::FormatTimeToString(format, TimeFromMicros(micros), out);
}

// FORMAT_TIMESTAMP(format, timestamp, time_zone).
absl::Status FormatTimestamp(std::string_view format, int64_t micros, std::string_view time_zone,
                             std::string* out) {
  return fn::FormatTimestampToString(format, micros, time_zone, kFormatOptions, out);
}

// PARSE_DATE(format, string).
absl::Status ParseDate(std::string_view format, std::string_view text, int32_t* out) {
  return fn::ParseStringToDate(format, text, /*parse_version2=*/true, out);
}

// PARSE_DATETIME(format, string).
absl::StatusOr<int64_t> ParseDatetime(std::string_view format, std::string_view text) {
  googlesql::DatetimeValue out;
  GOOGLESQL_RETURN_IF_ERROR(fn::ParseStringToDatetime(format, text, fn::kMicroseconds,
                                                      /*parse_version2=*/true, &out));
  return MicrosFromDatetime(out);
}

// PARSE_TIME(format, string).
absl::StatusOr<int64_t> ParseTime(std::string_view format, std::string_view text) {
  googlesql::TimeValue out;
  GOOGLESQL_RETURN_IF_ERROR(fn::ParseStringToTime(format, text, fn::kMicroseconds, &out));
  return MicrosFromTime(out);
}

// PARSE_TIMESTAMP(format, string, time_zone), with the time zone for a string without one.
absl::Status ParseTimestamp(std::string_view format, std::string_view text,
                            std::string_view time_zone, int64_t* out) {
  return fn::ParseStringToTimestamp(format, text, time_zone, /*parse_version2=*/true, out);
}

// CAST(string AS DATE), which accepts only BigQuery's canonical format, where DuckDB accepts more.
absl::Status StringToDate(std::string_view text, int32_t* out) {
  return fn::ConvertStringToDate(text, out);
}

// CAST(string AS DATETIME).
absl::StatusOr<int64_t> StringToDatetime(std::string_view text) {
  googlesql::DatetimeValue out;
  GOOGLESQL_RETURN_IF_ERROR(fn::ConvertStringToDatetime(text, fn::kMicroseconds, &out));
  return MicrosFromDatetime(out);
}

// CAST(string AS TIME).
absl::StatusOr<int64_t> StringToTime(std::string_view text) {
  googlesql::TimeValue out;
  GOOGLESQL_RETURN_IF_ERROR(fn::ConvertStringToTime(text, fn::kMicroseconds, &out));
  return MicrosFromTime(out);
}

// CAST(string AS TIMESTAMP), in UTC for a string without a time zone.
absl::Status StringToTimestamp(std::string_view text, int64_t* out) {
  return fn::ConvertStringToTimestamp(text, absl::UTCTimeZone(), fn::kMicroseconds,
                                      /*allow_tz_in_str=*/true, out);
}

// TIMESTAMP(string, time_zone), whose string may not have a time zone of its own.
absl::Status StringToTimestampIn(std::string_view text, std::string_view time_zone, int64_t* out) {
  return fn::ConvertStringToTimestamp(text, time_zone, fn::kMicroseconds,
                                      /*allow_tz_in_str=*/false, out);
}

// CAST(datetime AS STRING), whose fractional seconds have three or six digits, where DuckDB
// leaves out trailing zeros.
absl::Status DatetimeToString(int64_t micros, std::string* out) {
  GOOGLESQL_ASSIGN_OR_RETURN(const auto datetime, DatetimeFromMicros(micros));
  return fn::ConvertDatetimeToString(datetime, fn::kMicroseconds, out);
}

// CAST(time AS STRING).
absl::Status TimeToString(int64_t micros, std::string* out) {
  return fn::ConvertTimeToString(TimeFromMicros(micros), fn::kMicroseconds, out);
}

// CAST(timestamp AS STRING) and STRING(timestamp, time_zone), in the given time zone.
absl::Status TimestampToString(int64_t micros, std::string_view time_zone, std::string* out) {
  return fn::ConvertTimestampToString(absl::FromUnixMicros(micros), fn::kMicroseconds, time_zone,
                                      out);
}

// A FORMAT argument as the GoogleSQL value of the type its DuckDB type stands for.
absl::StatusOr<googlesql::Value> FormatArgument(const Arguments& arguments, idx_t column,
                                                duckdb_type type) {
  const bool null = arguments.IsNull(column);
  switch (type) {
    case DUCKDB_TYPE_VARCHAR:
      return null ? googlesql::Value::NullString()
                  : googlesql::Value::String(arguments.String(column));
    case DUCKDB_TYPE_BLOB:
      return null ? googlesql::Value::NullBytes()
                  : googlesql::Value::Bytes(arguments.String(column));
    case DUCKDB_TYPE_BIGINT:
      return null ? googlesql::Value::NullInt64() : googlesql::Value::Int64(arguments.Int(column));
    case DUCKDB_TYPE_DOUBLE:
      return null ? googlesql::Value::NullDouble()
                  : googlesql::Value::Double(arguments.Double(column));
    case DUCKDB_TYPE_BOOLEAN:
      return null ? googlesql::Value::NullBool() : googlesql::Value::Bool(arguments.Bool(column));
    case DUCKDB_TYPE_DATE:
      return null ? googlesql::Value::NullDate() : googlesql::Value::Date(arguments.Date(column));
    case DUCKDB_TYPE_TIME:
      return null ? googlesql::Value::NullTime()
                  : googlesql::Value::Time(TimeFromMicros(arguments.Int(column)));
    case DUCKDB_TYPE_TIMESTAMP: {
      if (null) {
        return googlesql::Value::NullDatetime();
      }
      const auto datetime = DatetimeFromMicros(arguments.Int(column));
      if (!datetime.ok()) {
        return datetime.status();
      }
      return googlesql::Value::Datetime(*datetime);
    }
    case DUCKDB_TYPE_TIMESTAMP_TZ:
      return null ? googlesql::Value::NullTimestamp()
                  : googlesql::Value::Timestamp(absl::FromUnixMicros(arguments.Int(column)));
    case DUCKDB_TYPE_BIGNUM: {
      if (null) {
        return googlesql::Value::NullBigNumeric();
      }
      const auto bignumeric = BigNumericFromBignum(arguments.String(column));
      if (!bignumeric.ok()) {
        return bignumeric.status();
      }
      return googlesql::Value::BigNumeric(*bignumeric);
    }
    case DUCKDB_TYPE_DECIMAL: {
      if (null) {
        return googlesql::Value::NullNumeric();
      }
      const auto numeric = googlesql::NumericValue::FromPackedInt(arguments.Decimal(column));
      if (!numeric.ok()) {
        return numeric.status();
      }
      return googlesql::Value::Numeric(*numeric);
    }
    default:
      return absl::UnimplementedError("Unsupported argument type for FORMAT");
  }
}

// FORMAT(format, values...), which is NULL for a NULL format and where GoogleSQL says so for
// a NULL value.
void Format(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  const std::vector<duckdb_type> types = ColumnTypes(input);
  EachRow(
      info, input, output,
      [&types](const Arguments& arguments) -> absl::StatusOr<std::optional<std::string>> {
        if (arguments.IsNull(0)) {
          return std::nullopt;
        }
        std::vector<googlesql::Value> values;
        for (idx_t column = 1; column < types.size(); ++column) {
          auto value = FormatArgument(arguments, column, types.at(column));
          if (!value.ok()) {
            return value.status();
          }
          values.push_back(*std::move(value));
        }
        std::string out;
        bool is_null = false;
        // BigQuery preserves the sign of floating-point zero in FORMAT.
        absl::Status status = googlesql::functions::StringFormatUtf8(
            arguments.String(0), values, googlesql::PRODUCT_EXTERNAL, &out, &is_null,
            /*canonicalize_zero=*/false, /*use_external_float32=*/true);
        if (!status.ok()) {
          return status;
        }
        return is_null ? std::nullopt : std::optional<std::string>(out);
      },
      /*nulls=*/false);
}

}  // namespace

void RegisterDatetimeFunctions(duckdb_connection connection) {
  Register(connection, "bq_format_date", {kVarchar, kDate}, kVarchar, Apply<FormatDate>);
  Register(connection, "bq_format_datetime", {kVarchar, kDatetime}, kVarchar,
           Apply<FormatDatetime>);
  Register(connection, "bq_format_time", {kVarchar, kTime}, kVarchar, Apply<FormatTime>);
  Register(connection, "bq_format_timestamp", {kVarchar, kTimestamp, kVarchar}, kVarchar,
           Apply<FormatTimestamp>);
  Register(connection, "bq_parse_date", {kVarchar, kVarchar}, kDate, Apply<ParseDate>);
  Register(connection, "bq_parse_datetime", {kVarchar, kVarchar}, kDatetime, Apply<ParseDatetime>);
  Register(connection, "bq_parse_time", {kVarchar, kVarchar}, kTime, Apply<ParseTime>);
  Register(connection, "bq_parse_timestamp", {kVarchar, kVarchar, kVarchar}, kTimestamp,
           Apply<ParseTimestamp>);
  Register(connection, "bq_string_to_date", {kVarchar, kBoolean}, kDate,
           Apply<StringToDate, Errors::kNullUnderSafeCast>);
  Register(connection, "bq_string_to_datetime", {kVarchar, kBoolean}, kDatetime,
           Apply<StringToDatetime, Errors::kNullUnderSafeCast>);
  Register(connection, "bq_string_to_time", {kVarchar, kBoolean}, kTime,
           Apply<StringToTime, Errors::kNullUnderSafeCast>);
  Register(connection, "bq_string_to_timestamp", {kVarchar, kBoolean}, kTimestamp,
           Apply<StringToTimestamp, Errors::kNullUnderSafeCast>);
  Register(connection, "bq_string_to_timestamp_in", {kVarchar, kVarchar}, kTimestamp,
           Apply<StringToTimestampIn>);
  Register(connection, "bq_datetime_string", {kDatetime}, kVarchar, Apply<DatetimeToString>);
  Register(connection, "bq_time_string", {kTime}, kVarchar, Apply<TimeToString>);
  Register(connection, "bq_timestamp_string", {kTimestamp, kVarchar}, kVarchar,
           Apply<TimestampToString>);
  Register(connection, "bq_format", {kVarchar}, kVarchar, Format, /*nulls=*/false, DUCKDB_TYPE_ANY);
}

}  // namespace bigquery_emulator_duckdb::backend_functions
