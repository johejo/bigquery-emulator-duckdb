#include "src/googlesql_functions/cast.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "absl/status/statusor.h"
#include "absl/time/time.h"
#include "googlesql/base/status_macros.h"
#include "googlesql/public/cast.h"
#include "googlesql/public/timestamp_picos_value.h"
#include "googlesql/public/types/type_factory.h"
#include "googlesql/public/value.h"
#include "src/googlesql_functions/datetime.h"
#include "src/language_options.h"

namespace googlesql_functions {

namespace types = googlesql::types;
using googlesql::BigNumericValue;
using googlesql::NumericValue;
using googlesql::Value;

namespace {

// CAST(value AS to FORMAT format [AT TIME ZONE time_zone]), by GoogleSQL's conversion, in UTC
// unless `time_zone` says otherwise. `now`, in microseconds, fills in the year and month a
// format for parsing leaves out.
absl::StatusOr<Value> FormatCast(const Value& value, const googlesql::Type* to,
                                 std::string_view format,
                                 std::optional<std::string_view> time_zone = std::nullopt,
                                 std::optional<int64_t> now = std::nullopt) {
  return googlesql::internal::CastValueWithoutTypeValidation(
      value, absl::UTCTimeZone(),
      now.has_value() ? std::optional<absl::Time>(absl::FromUnixMicros(*now)) : std::nullopt,
      bigquery_emulator_duckdb::GoogleSqlLanguageOptions(), to, std::string(format),
      time_zone.has_value() ? std::optional<std::string>(*time_zone) : std::nullopt,
      /*extended_conversion_evaluator=*/nullptr, /*canonicalize_zero=*/false);
}

absl::StatusOr<std::string> FormatString(const Value& value, std::string_view format,
                                         std::optional<std::string_view> time_zone = std::nullopt) {
  GOOGLESQL_ASSIGN_OR_RETURN(const Value out,
                             FormatCast(value, types::StringType(), format, time_zone));
  return out.string_value();
}

}  // namespace

// CAST(bytes AS STRING), which fails for bytes that are not UTF-8.
absl::StatusOr<std::string> CastBytesToUtf8(std::string_view bytes) {
  GOOGLESQL_ASSIGN_OR_RETURN(
      const Value out, googlesql::CastValue(Value::Bytes(bytes), absl::UTCTimeZone(),
                                            bigquery_emulator_duckdb::GoogleSqlLanguageOptions(),
                                            types::StringType()));
  return out.string_value();
}

// Keep the conversion's range checks and error text, including values at the INT64 limits.
absl::StatusOr<int64_t> CastFloat64ToInt64(double value) {
  GOOGLESQL_ASSIGN_OR_RETURN(
      const Value out, googlesql::CastValue(Value::Double(value), absl::UTCTimeZone(),
                                            bigquery_emulator_duckdb::GoogleSqlLanguageOptions(),
                                            types::Int64Type()));
  return out.int64_value();
}

// BigQuery accepts only case-insensitive "true" and "false", unlike DuckDB's BOOL cast.
absl::StatusOr<bool> CastStringToBool(std::string_view text) {
  GOOGLESQL_ASSIGN_OR_RETURN(
      const Value out, googlesql::CastValue(Value::String(text), absl::UTCTimeZone(),
                                            bigquery_emulator_duckdb::GoogleSqlLanguageOptions(),
                                            types::BoolType()));
  return out.bool_value();
}

absl::StatusOr<std::string> CastBytesToString(std::string_view bytes, std::string_view format) {
  return FormatString(Value::Bytes(bytes), format);
}

absl::StatusOr<std::string> CastDateToString(int32_t date, std::string_view format) {
  return FormatString(Value::Date(date), format);
}

absl::StatusOr<std::string> CastDatetimeToString(int64_t micros, std::string_view format) {
  GOOGLESQL_ASSIGN_OR_RETURN(const auto datetime, DatetimeFromMicros(micros));
  return FormatString(Value::Datetime(datetime), format);
}

absl::StatusOr<std::string> CastTimeToString(int64_t micros, std::string_view format) {
  return FormatString(Value::Time(TimeFromMicros(micros)), format);
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the format and the time zone.
absl::StatusOr<std::string> CastTimestampToString(int64_t micros, std::string_view format,
                                                  std::string_view time_zone) {
  return FormatString(Value::Timestamp(absl::FromUnixMicros(micros)), format, time_zone);
}

absl::StatusOr<std::string> CastInt64ToString(int64_t value, std::string_view format) {
  return FormatString(Value::Int64(value), format);
}

absl::StatusOr<std::string> CastFloat64ToString(double value, std::string_view format) {
  return FormatString(Value::Double(value), format);
}

absl::StatusOr<std::string> CastNumericToString(NumericValue value, std::string_view format) {
  return FormatString(Value::Numeric(value), format);
}

absl::StatusOr<std::string> CastBigNumericToString(BigNumericValue value, std::string_view format) {
  return FormatString(Value::BigNumeric(value), format);
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the string and its format.
absl::StatusOr<std::string> CastStringToBytes(std::string_view text, std::string_view format) {
  GOOGLESQL_ASSIGN_OR_RETURN(const Value out,
                             FormatCast(Value::String(text), types::BytesType(), format));
  return out.bytes_value();
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the string and its format.
absl::StatusOr<int32_t> CastStringToDate(std::string_view text, std::string_view format,
                                         int64_t now) {
  GOOGLESQL_ASSIGN_OR_RETURN(const Value out, FormatCast(Value::String(text), types::DateType(),
                                                         format, std::nullopt, now));
  return out.date_value();
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the string and its format.
absl::StatusOr<int64_t> CastStringToDatetime(std::string_view text, std::string_view format,
                                             int64_t now) {
  GOOGLESQL_ASSIGN_OR_RETURN(const Value out, FormatCast(Value::String(text), types::DatetimeType(),
                                                         format, std::nullopt, now));
  return MicrosFromDatetime(out.datetime_value());
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the string and its format.
absl::StatusOr<int64_t> CastStringToTime(std::string_view text, std::string_view format) {
  GOOGLESQL_ASSIGN_OR_RETURN(const Value out,
                             FormatCast(Value::String(text), types::TimeType(), format));
  return MicrosFromTime(out.time_value());
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the string, its format and time zone.
absl::StatusOr<int64_t> CastStringToTimestamp(std::string_view text, std::string_view format,
                                              std::string_view time_zone, int64_t now) {
  GOOGLESQL_ASSIGN_OR_RETURN(
      const Value out,
      FormatCast(Value::String(text), types::TimestampType(), format, time_zone, now));
  return absl::ToUnixMicros(out.ToUnixPicos().ToAbslTime());
}

}  // namespace googlesql_functions
