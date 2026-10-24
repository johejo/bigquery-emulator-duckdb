#include "src/googlesql_functions/datetime.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/ascii.h"
#include "absl/time/civil_time.h"
#include "absl/time/time.h"
#include "googlesql/base/status_macros.h"
#include "googlesql/public/functions/date_time_util.h"
#include "googlesql/public/functions/datetime.pb.h"
#include "googlesql/public/functions/generate_array.h"
#include "googlesql/public/functions/parse_date_time.h"

namespace googlesql_functions {

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
  absl::Status status = googlesql::functions::ConvertTimestampToDatetime(
      absl::FromUnixMicros(micros), absl::UTCTimeZone(), &datetime);
  if (!status.ok()) {
    return status;
  }
  return datetime;
}

absl::StatusOr<int64_t> MicrosFromDatetime(const googlesql::DatetimeValue& datetime) {
  absl::Time time;
  absl::Status status =
      googlesql::functions::ConvertDatetimeToTimestamp(datetime, absl::UTCTimeZone(), &time);
  if (!status.ok()) {
    return status;
  }
  return absl::ToUnixMicros(time);
}

namespace fn = googlesql::functions;

namespace {

// BigQuery expands %Q to the quarter and %J to the ISO day of the year.
constexpr fn::FormatDateTimestampOptions kFormatOptions = {
    .expand_Q = true,
    .expand_J = true,
};

// Expands %G, the ISO 8601 year, in format for day. GoogleSQL leaves it to the C library's
// strftime(), and macOS pads years below 1000 with zeros where BigQuery, like glibc, does not.
std::string ExpandIsoYear(std::string_view format, absl::CivilDay day) {
  // The ISO year is the year of the week's Thursday; absl::Weekday counts from Monday.
  const absl::CivilDay thursday = day + (3 - static_cast<int>(absl::GetWeekday(day)));
  std::string expanded;
  for (size_t pct = format.find('%'); pct != std::string_view::npos && pct + 1 < format.size();
       pct = format.find('%')) {
    expanded.append(format.substr(0, pct));
    const std::string_view element = format.substr(pct, 2);
    expanded += element == "%G" ? std::to_string(thursday.year()) : std::string(element);
    format.remove_prefix(pct + 2);
  }
  expanded.append(format);
  return expanded;
}

}  // namespace

// FORMAT_DATE(format, date).
absl::Status FormatDate(std::string_view format, int32_t date, std::string* out) {
  if (format.find("%G") != std::string_view::npos) {
    return fn::FormatDateToString(ExpandIsoYear(format, absl::CivilDay(1970, 1, 1) + date), date,
                                  kFormatOptions, out);
  }
  return fn::FormatDateToString(format, date, kFormatOptions, out);
}

// FORMAT_DATETIME(format, datetime).
absl::Status FormatDatetime(std::string_view format, int64_t micros, std::string* out) {
  GOOGLESQL_ASSIGN_OR_RETURN(const auto datetime, DatetimeFromMicros(micros));
  if (format.find("%G") != std::string_view::npos) {
    return fn::FormatDatetimeToStringWithOptions(
        ExpandIsoYear(format, absl::ToCivilDay(absl::FromUnixMicros(micros), absl::UTCTimeZone())),
        datetime, kFormatOptions, out);
  }
  return fn::FormatDatetimeToStringWithOptions(format, datetime, kFormatOptions, out);
}

// FORMAT_TIME(format, time).
absl::Status FormatTime(std::string_view format, int64_t micros, std::string* out) {
  return fn::FormatTimeToString(format, TimeFromMicros(micros), out);
}

// FORMAT_TIMESTAMP(format, timestamp, time_zone).
absl::Status FormatTimestamp(std::string_view format, int64_t micros, std::string_view time_zone,
                             std::string* out) {
  absl::TimeZone zone;
  if (format.find("%G") != std::string_view::npos && fn::MakeTimeZone(time_zone, &zone).ok()) {
    return fn::FormatTimestampToString(
        ExpandIsoYear(format, absl::ToCivilDay(absl::FromUnixMicros(micros), zone)), micros,
        time_zone, kFormatOptions, out);
  }
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

// The civil time of a TIMESTAMP in a time zone, which may be a UTC offset such as '-08:00' that
// DuckDB does not accept.
absl::Status TimestampToDatetimeIn(int64_t micros, std::string_view time_zone, int64_t* out) {
  googlesql::DatetimeValue datetime;
  GOOGLESQL_RETURN_IF_ERROR(
      fn::ConvertTimestampToDatetime(absl::FromUnixMicros(micros), time_zone, &datetime));
  GOOGLESQL_ASSIGN_OR_RETURN(*out, MicrosFromDatetime(datetime));
  return absl::OkStatus();
}

// TIME(timestamp, time_zone), whose date may be out of the DATETIME range.
absl::Status TimestampToTimeIn(int64_t micros, std::string_view time_zone, int64_t* out) {
  googlesql::TimeValue time;
  GOOGLESQL_RETURN_IF_ERROR(fn::ConvertTimestampToTime(absl::FromUnixMicros(micros), time_zone,
                                                       fn::kMicroseconds, &time));
  *out = MicrosFromTime(time);
  return absl::OkStatus();
}

// The civil time of a TIMESTAMP in a time zone for EXTRACT, which, like GoogleSQL's, reads any
// part but DATE of a civil time out of the DATETIME range, such as in year 0.
absl::Status TimestampCivilIn(int64_t micros, std::string_view time_zone, int64_t* out) {
  absl::TimeZone zone;
  GOOGLESQL_RETURN_IF_ERROR(fn::MakeTimeZone(time_zone, &zone));
  constexpr int64_t kPerSecond = 1000000;
  const absl::CivilSecond civil = zone.At(absl::FromUnixMicros(micros)).cs;
  // Offsets are whole seconds, so the fraction of a second is the instant's.
  *out = ((civil - absl::CivilSecond(1970)) * kPerSecond) +
         (((micros % kPerSecond) + kPerSecond) % kPerSecond);
  return absl::OkStatus();
}

// TIMESTAMP(datetime, time_zone), which takes the earlier of a repeated civil time, where
// DuckDB's timezone() takes the later.
absl::Status DatetimeToTimestampIn(int64_t micros, std::string_view time_zone, int64_t* out) {
  GOOGLESQL_ASSIGN_OR_RETURN(const auto datetime, DatetimeFromMicros(micros));
  absl::Time time;
  GOOGLESQL_RETURN_IF_ERROR(fn::ConvertDatetimeToTimestamp(datetime, time_zone, &time));
  *out = absl::ToUnixMicros(time);
  return absl::OkStatus();
}

// TIMESTAMP_TRUNC, DATE_TRUNC and DATETIME_TRUNC of a TIMESTAMP to a date part in a time zone,
// such as 'day' or 'week_monday'.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the date part and the time zone.
absl::Status TimestampTrunc(int64_t micros, std::string_view part, std::string_view time_zone,
                            int64_t* out) {
  fn::DateTimestampPart value = fn::DATE;
  if (!fn::DateTimestampPart_Parse(absl::AsciiStrToUpper(part), &value)) {
    return absl::InvalidArgumentError("Unsupported date part");
  }
  return fn::TimestampTrunc(micros, time_zone, value, out);
}

// GENERATE_DATE_ARRAY(start, end, INTERVAL step part), with BigQuery's documented
// 1,048,576-element limit. Like BigQuery, each step adds to the previous element, so an element
// clamped to the end of a shorter month moves the rest to that day.
absl::Status GenerateDateArray(int32_t start, int32_t end, int64_t step, std::string_view part,
                               std::vector<int32_t>* out) {
  constexpr int kMaxGeneratedArraySize = 1048576;
  fn::DateTimestampPart unit = fn::DAY;
  if (!fn::DateTimestampPart_Parse(absl::AsciiStrToUpper(part), &unit)) {
    return absl::InvalidArgumentError("Unsupported date part");
  }
  std::vector<int64_t> dates;
  GOOGLESQL_RETURN_IF_ERROR((fn::GenerateArrayHelper<fn::ArrayGenTrait<int64_t, fn::DateIncrement>,
                                                     kMaxGeneratedArraySize>(
      start, end, {.unit = unit, .value = step}, &dates)));
  out->assign(dates.begin(), dates.end());
  return absl::OkStatus();
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

}  // namespace googlesql_functions
