#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/time/time.h"
#include "duckdb.h"
#include "googlesql/public/civil_time.h"
#include "googlesql/public/functions/date_time_util.h"
#include "googlesql/public/functions/parse_date_time.h"
#include "googlesql/public/functions/string_format.h"
#include "googlesql/public/options.pb.h"
#include "googlesql/public/value.h"
#include "src/backend_functions/internal.h"
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
    LogicalType type(duckdb_vector_get_column_type(duckdb_data_chunk_get_vector(input, column)));
    types.push_back(duckdb_get_type_id(type.get()));
  }
  return types;
}

// BigQuery expands %Q to the quarter and %J to the ISO day of the year.
constexpr googlesql::functions::FormatDateTimestampOptions kFormatOptions = {.expand_Q = true,
                                                                             .expand_J = true};

// FORMAT_DATE(format, date).
void FormatDate(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  EachRow(info, input, output, [](const Arguments& arguments) {
    std::string out;
    const absl::Status status = googlesql::functions::FormatDateToString(
        arguments.String(0), int64_t{arguments.Date(1)}, kFormatOptions, &out);
    return ToStatusOr(status.ok(), out, status);
  });
}

// FORMAT_DATETIME(format, datetime).
void FormatDatetime(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  EachRow(info, input, output, [](const Arguments& arguments) -> absl::StatusOr<std::string> {
    const auto datetime = DatetimeFromMicros(arguments.Int(1));
    if (!datetime.ok()) {
      return datetime.status();
    }
    std::string out;
    const absl::Status status = googlesql::functions::FormatDatetimeToStringWithOptions(
        arguments.String(0), *datetime, kFormatOptions, &out);
    return ToStatusOr(status.ok(), out, status);
  });
}

// FORMAT_TIME(format, time).
void FormatTime(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  EachRow(info, input, output, [](const Arguments& arguments) {
    std::string out;
    const absl::Status status = googlesql::functions::FormatTimeToString(
        arguments.String(0), TimeFromMicros(arguments.Int(1)), &out);
    return ToStatusOr(status.ok(), out, status);
  });
}

// FORMAT_TIMESTAMP(format, timestamp, time_zone).
void FormatTimestamp(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  EachRow(info, input, output, [](const Arguments& arguments) {
    std::string out;
    const absl::Status status = googlesql::functions::FormatTimestampToString(
        arguments.String(0), arguments.Int(1), arguments.String(2), kFormatOptions, &out);
    return ToStatusOr(status.ok(), out, status);
  });
}

// PARSE_DATE(format, string).
void ParseDate(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  EachRow(info, input, output, [](const Arguments& arguments) {
    int32_t out = 0;
    const absl::Status status = googlesql::functions::ParseStringToDate(
        arguments.String(0), arguments.String(1), /*parse_version2=*/true, &out);
    return ToStatusOr(status.ok(), out, status);
  });
}

// PARSE_DATETIME(format, string).
void ParseDatetime(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  EachRow(info, input, output, [](const Arguments& arguments) -> absl::StatusOr<int64_t> {
    googlesql::DatetimeValue out;
    absl::Status status = googlesql::functions::ParseStringToDatetime(
        arguments.String(0), arguments.String(1), googlesql::functions::kMicroseconds,
        /*parse_version2=*/true, &out);
    if (!status.ok()) {
      return status;
    }
    return MicrosFromDatetime(out);
  });
}

// PARSE_TIME(format, string).
void ParseTime(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  EachRow(info, input, output, [](const Arguments& arguments) -> absl::StatusOr<int64_t> {
    googlesql::TimeValue out;
    absl::Status status = googlesql::functions::ParseStringToTime(
        arguments.String(0), arguments.String(1), googlesql::functions::kMicroseconds, &out);
    if (!status.ok()) {
      return status;
    }
    return MicrosFromTime(out);
  });
}

// PARSE_TIMESTAMP(format, string, time_zone), with the time zone for a string without one.
void ParseTimestamp(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  EachRow(info, input, output, [](const Arguments& arguments) {
    int64_t out = 0;
    const absl::Status status = googlesql::functions::ParseStringToTimestamp(
        arguments.String(0), arguments.String(1), arguments.String(2), /*parse_version2=*/true,
        &out);
    return ToStatusOr(status.ok(), out, status);
  });
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
          auto value = FormatArgument(arguments, column, types[column]);
          if (!value.ok()) {
            return value.status();
          }
          values.push_back(*std::move(value));
        }
        std::string out;
        bool is_null = false;
        absl::Status status = googlesql::functions::StringFormatUtf8(
            arguments.String(0), values, googlesql::PRODUCT_EXTERNAL, &out, &is_null,
            /*canonicalize_zero=*/true, /*use_external_float32=*/true);
        if (!status.ok()) {
          return status;
        }
        return is_null ? std::nullopt : std::optional<std::string>(out);
      },
      /*nulls=*/false);
}

}  // namespace

void RegisterDatetimeFunctions(duckdb_connection connection) {
  Register(connection, "bq_format_date", {kVarchar, kDate}, kVarchar, FormatDate);
  Register(connection, "bq_format_datetime", {kVarchar, kDatetime}, kVarchar, FormatDatetime);
  Register(connection, "bq_format_time", {kVarchar, kTime}, kVarchar, FormatTime);
  Register(connection, "bq_format_timestamp", {kVarchar, kTimestamp, kVarchar}, kVarchar,
           FormatTimestamp);
  Register(connection, "bq_parse_date", {kVarchar, kVarchar}, kDate, ParseDate);
  Register(connection, "bq_parse_datetime", {kVarchar, kVarchar}, kDatetime, ParseDatetime);
  Register(connection, "bq_parse_time", {kVarchar, kVarchar}, kTime, ParseTime);
  Register(connection, "bq_parse_timestamp", {kVarchar, kVarchar, kVarchar}, kTimestamp,
           ParseTimestamp);
  Register(connection, "bq_format", {kVarchar}, kVarchar, Format, /*nulls=*/false, DUCKDB_TYPE_ANY);
}

}  // namespace bigquery_emulator_duckdb::backend_functions
