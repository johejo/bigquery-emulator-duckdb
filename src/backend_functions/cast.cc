#include "src/backend_functions/cast.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "absl/status/statusor.h"
#include "absl/time/time.h"
#include "duckdb.h"
#include "googlesql/base/status_macros.h"
#include "googlesql/public/cast.h"
#include "googlesql/public/numeric_value.h"
#include "googlesql/public/timestamp_picos_value.h"
#include "googlesql/public/type.h"
#include "googlesql/public/types/type_factory.h"
#include "googlesql/public/value.h"
#include "src/backend_functions/apply.h"
#include "src/backend_functions/datetime.h"
#include "src/backend_functions/register.h"
#include "src/backend_functions/scalar.h"
#include "src/duckdb_handle.h"
#include "src/language_options.h"

namespace bigquery_emulator_duckdb::backend_functions {
namespace {

namespace types = googlesql::types;
using googlesql::BigNumericValue;
using googlesql::NumericValue;
using googlesql::Value;

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
      GoogleSqlLanguageOptions(), to, std::string(format),
      time_zone.has_value() ? std::optional<std::string>(*time_zone) : std::nullopt,
      /*extended_conversion_evaluator=*/nullptr, /*canonicalize_zero=*/false);
}

absl::StatusOr<std::string> FormatString(const Value& value, std::string_view format,
                                         std::optional<std::string_view> time_zone = std::nullopt) {
  GOOGLESQL_ASSIGN_OR_RETURN(const Value out,
                             FormatCast(value, types::StringType(), format, time_zone));
  return out.string_value();
}

// CAST(bytes AS STRING), which fails for bytes that are not UTF-8.
absl::StatusOr<std::string> BytesToUtf8(std::string_view bytes) {
  GOOGLESQL_ASSIGN_OR_RETURN(const Value out,
                             googlesql::CastValue(Value::Bytes(bytes), absl::UTCTimeZone(),
                                                  GoogleSqlLanguageOptions(), types::StringType()));
  return out.string_value();
}

absl::StatusOr<std::string> BytesToString(std::string_view bytes, std::string_view format) {
  return FormatString(Value::Bytes(bytes), format);
}

absl::StatusOr<std::string> DateToString(int32_t date, std::string_view format) {
  return FormatString(Value::Date(date), format);
}

absl::StatusOr<std::string> DatetimeToString(int64_t micros, std::string_view format) {
  GOOGLESQL_ASSIGN_OR_RETURN(const auto datetime, DatetimeFromMicros(micros));
  return FormatString(Value::Datetime(datetime), format);
}

absl::StatusOr<std::string> TimeToString(int64_t micros, std::string_view format) {
  return FormatString(Value::Time(TimeFromMicros(micros)), format);
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the format and the time zone.
absl::StatusOr<std::string> TimestampToString(int64_t micros, std::string_view format,
                                              std::string_view time_zone) {
  return FormatString(Value::Timestamp(absl::FromUnixMicros(micros)), format, time_zone);
}

absl::StatusOr<std::string> Int64ToString(int64_t value, std::string_view format) {
  return FormatString(Value::Int64(value), format);
}

absl::StatusOr<std::string> Float64ToString(double value, std::string_view format) {
  return FormatString(Value::Double(value), format);
}

absl::StatusOr<std::string> NumericToString(NumericValue value, std::string_view format) {
  return FormatString(Value::Numeric(value), format);
}

absl::StatusOr<std::string> BigNumericToString(BigNumericValue value, std::string_view format) {
  return FormatString(Value::BigNumeric(value), format);
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the string and its format.
absl::StatusOr<std::string> StringToBytes(std::string_view text, std::string_view format) {
  GOOGLESQL_ASSIGN_OR_RETURN(const Value out,
                             FormatCast(Value::String(text), types::BytesType(), format));
  return out.bytes_value();
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the string and its format.
absl::StatusOr<int32_t> StringToDate(std::string_view text, std::string_view format, int64_t now) {
  GOOGLESQL_ASSIGN_OR_RETURN(const Value out, FormatCast(Value::String(text), types::DateType(),
                                                         format, std::nullopt, now));
  return out.date_value();
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the string and its format.
absl::StatusOr<int64_t> StringToDatetime(std::string_view text, std::string_view format,
                                         int64_t now) {
  GOOGLESQL_ASSIGN_OR_RETURN(const Value out, FormatCast(Value::String(text), types::DatetimeType(),
                                                         format, std::nullopt, now));
  return MicrosFromDatetime(out.datetime_value());
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the string and its format.
absl::StatusOr<int64_t> StringToTime(std::string_view text, std::string_view format) {
  GOOGLESQL_ASSIGN_OR_RETURN(const Value out,
                             FormatCast(Value::String(text), types::TimeType(), format));
  return MicrosFromTime(out.time_value());
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the string, its format and time zone.
absl::StatusOr<int64_t> StringToTimestamp(std::string_view text, std::string_view format,
                                          std::string_view time_zone, int64_t now) {
  GOOGLESQL_ASSIGN_OR_RETURN(
      const Value out,
      FormatCast(Value::String(text), types::TimestampType(), format, time_zone, now));
  return absl::ToUnixMicros(out.ToUnixPicos().ToAbslTime());
}

}  // namespace

void RegisterCastFunctions(duckdb_connection connection) {
  constexpr Errors kSafe = Errors::kNullUnderSafeCast;
  LogicalType const numeric(duckdb_create_decimal_type(38, 9));
  LogicalType const varchar(duckdb_create_logical_type(kVarchar));
  LogicalType const boolean(duckdb_create_logical_type(kBoolean));
  Register(connection, "bq_bytes_to_string", {kBlob, kBoolean}, kVarchar,
           Apply<BytesToUtf8, kSafe>);
  // Each cast with FORMAT takes the value, the format, the time zone for a cast from or to
  // TIMESTAMP, the current timestamp for a cast to DATE, DATETIME or TIMESTAMP and whether it is
  // SAFE_CAST.
  Register(connection, "bq_cast_bytes_to_string", {kBlob, kVarchar, kBoolean}, kVarchar,
           Apply<BytesToString, kSafe>);
  Register(connection, "bq_cast_date_to_string", {kDate, kVarchar, kBoolean}, kVarchar,
           Apply<DateToString, kSafe>);
  Register(connection, "bq_cast_datetime_to_string", {kDatetime, kVarchar, kBoolean}, kVarchar,
           Apply<DatetimeToString, kSafe>);
  Register(connection, "bq_cast_time_to_string", {kTime, kVarchar, kBoolean}, kVarchar,
           Apply<TimeToString, kSafe>);
  Register(connection, "bq_cast_timestamp_to_string", {kTimestamp, kVarchar, kVarchar, kBoolean},
           kVarchar, Apply<TimestampToString, kSafe>);
  Register(connection, "bq_cast_int64_to_string", {kBigint, kVarchar, kBoolean}, kVarchar,
           Apply<Int64ToString, kSafe>);
  Register(connection, "bq_cast_float64_to_string", {kDouble, kVarchar, kBoolean}, kVarchar,
           Apply<Float64ToString, kSafe>);
  Register(connection, "bq_cast_numeric_to_string", {numeric.get(), varchar.get(), boolean.get()},
           varchar.get(), Apply<NumericToString, kSafe>);
  // A BIGNUMERIC is the VARCHAR of its units; see src/bignumeric.h.
  Register(connection, "bq_cast_bignumeric_to_string", {kVarchar, kVarchar, kBoolean}, kVarchar,
           Apply<BigNumericToString, kSafe>);
  Register(connection, "bq_cast_string_to_bytes", {kVarchar, kVarchar, kBoolean}, kBlob,
           Apply<StringToBytes, kSafe>);
  Register(connection, "bq_cast_string_to_date", {kVarchar, kVarchar, kTimestamp, kBoolean}, kDate,
           Apply<StringToDate, kSafe>);
  Register(connection, "bq_cast_string_to_datetime", {kVarchar, kVarchar, kTimestamp, kBoolean},
           kDatetime, Apply<StringToDatetime, kSafe>);
  Register(connection, "bq_cast_string_to_time", {kVarchar, kVarchar, kBoolean}, kTime,
           Apply<StringToTime, kSafe>);
  Register(connection, "bq_cast_string_to_timestamp",
           {kVarchar, kVarchar, kVarchar, kTimestamp, kBoolean}, kTimestamp,
           Apply<StringToTimestamp, kSafe>);
}

}  // namespace bigquery_emulator_duckdb::backend_functions
