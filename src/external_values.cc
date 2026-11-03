#include "src/external_values.h"

#include <cctype>
#include <cstdint>
#include <regex>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/ascii.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_replace.h"
#include "absl/time/civil_time.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "googlesql/base/status_macros.h"
#include "googlesql/public/civil_time.h"
#include "googlesql/public/functions/cast_date_time.h"
#include "googlesql/public/functions/date_time_util.h"
#include "googlesql/public/numeric_value.h"
#include "googlesql/public/type.pb.h"
#include "nlohmann/json.hpp"
#include "src/api_error.h"
#include "src/field_schema.h"
#include "src/file_read_options.h"
#include "src/googlesql_geography/input.h"

namespace bigquery_emulator_duckdb {
namespace {

absl::StatusOr<std::string> Datetime(std::string_view text) {
  const auto invalid = [text] {
    return absl::InvalidArgumentError("Invalid datetime string \"" + std::string(text) + "\"");
  };
  if (text.empty() || std::isspace(static_cast<unsigned char>(text.front())) != 0 ||
      std::isspace(static_cast<unsigned char>(text.back())) != 0) {
    return invalid();
  }
  googlesql::DatetimeValue datetime;
  if (!googlesql::functions::ConvertStringToDatetime(text, googlesql::functions::kMicroseconds,
                                                     &datetime)
           .ok()) {
    return invalid();
  }
  std::string out;
  if (!googlesql::functions::ConvertDatetimeToString(datetime, googlesql::functions::kMicroseconds,
                                                     &out)
           .ok()) {
    return invalid();
  }
  return out;
}

absl::StatusOr<std::string> BigNumeric(std::string_view text) {
  const auto value = googlesql::BigNumericValue::FromStringStrict(absl::StripAsciiWhitespace(text));
  if (!value.ok()) {
    return absl::InvalidArgumentError("Invalid BIGNUMERIC value: " + std::string(text));
  }
  return value->ToString();
}

absl::StatusOr<std::string> Json(std::string_view text) {
  using nlohmann::json;
  // The keys of each enclosing object, of which the first of duplicates is kept.
  std::vector<std::set<std::string>> keys;
  const auto first_keys = [&keys](int /*depth*/, json::parse_event_t event, json& parsed) {
    if (event == json::parse_event_t::object_start) {
      keys.emplace_back();
    } else if (event == json::parse_event_t::object_end) {
      keys.pop_back();
    } else if (event == json::parse_event_t::key) {
      return keys.back().insert(parsed.get<std::string>()).second;
    }
    return true;
  };
  try {
    return json::parse(text, first_keys).dump();
  } catch (const json::parse_error& error) {
    // nlohmann prefixes its message with the exception's id and position.
    std::string_view message = error.what();
    if (const auto colon = message.find(": "); colon != std::string_view::npos) {
      message.remove_prefix(colon + 2);
    }
    return absl::InvalidArgumentError(message);
  }
}

absl::StatusOr<std::string> Geography(std::string_view text) {
  constexpr std::string_view kSrid = "SRID=4326;";
  std::string_view geography = text;
  if (geography.starts_with(kSrid)) {
    geography.remove_prefix(kSrid.size());
  }
  const auto read = googlesql_geography::ReadGeography(geography, /*oriented=*/true, "");
  if (!read.ok()) {
    return read.status();
  }
  return std::string(geography);
}

// Today in UTC, of which CAST ... FORMAT takes the parts a format leaves out.
int32_t CurrentDate() {
  return static_cast<int32_t>(absl::ToCivilDay(absl::Now(), absl::UTCTimeZone()) -
                              absl::CivilDay(1970, 1, 1));
}

// The UTC text DuckDB reads as the TIMESTAMP at `time`.
std::string TimestampText(absl::Time time) {
  return absl::FormatTime("%Y-%m-%d %H:%M:%E6S+00", time, absl::UTCTimeZone());
}

absl::StatusOr<absl::TimeZone> TimeZone(const std::string& name) {
  absl::TimeZone zone = absl::UTCTimeZone();
  if (!name.empty() && !absl::LoadTimeZone(name, &zone)) {
    return absl::InvalidArgumentError("time_zone " + name + " is not valid.");
  }
  return zone;
}

// The TIMESTAMP `text` in the forms of the CSV data types, without a UTC offset in `zone`.
absl::StatusOr<std::string> ZonedTimestamp(std::string_view text, absl::TimeZone zone) {
  static const std::regex pattern(
      R"(([0-9]{4})[-/]([0-9]{1,2})[-/]([0-9]{1,2})[ T]([0-9]{1,2}):([0-9]{2})(?::([0-9]{2})(\.[0-9]{1,6})?)? *(Z|UTC|[+-][0-9]{1,2}(?::?[0-9]{2})?)?)");
  const auto unparsable = [text] {
    return absl::InvalidArgumentError(
        "Could not parse '" + std::string(text) +
        "' as a timestamp. Required format is YYYY-MM-DD HH:MM[:SS[.SSSSSS]] or "
        "YYYY/MM/DD HH:MM[:SS[.SSSSSS]]");
  };
  std::cmatch match;
  if (!std::regex_match(text.begin(), text.end(), match, pattern)) {
    static const std::regex epoch(R"([0-9]+(\.[0-9]*)?([eE][+-]?[0-9]+)?)");
    if (std::regex_match(text.begin(), text.end(), epoch)) {
      return absl::UnimplementedError(std::string(kUnsupported) +
                                      "Unix epoch TIMESTAMP values with time_zone");
    }
    return unparsable();
  }
  // An unmatched group is empty.
  const auto number = [&match](int group) {
    const std::string digits = match.str(group);
    return digits.empty() ? 0 : std::stoi(digits);
  };
  const absl::CivilSecond civil(number(1), number(2), number(3), number(4), number(5), number(6));
  if (civil.year() != number(1) || civil.month() != number(2) || civil.day() != number(3) ||
      civil.hour() != number(4) || civil.minute() != number(5) || civil.second() != number(6) ||
      number(1) == 0) {
    return unparsable();
  }
  const std::string offset = match.str(8);
  if (offset == "Z" || offset == "UTC") {
    zone = absl::UTCTimeZone();
  } else if (!offset.empty()) {
    const int sign = offset.front() == '-' ? -1 : 1;
    const std::string digits = absl::StrReplaceAll(offset.substr(1), {{":", ""}});
    const int hours =
        std::stoi(digits.substr(0, digits.size() > 2 ? digits.size() - 2 : digits.size()));
    const int minutes = digits.size() > 2 ? std::stoi(digits.substr(digits.size() - 2)) : 0;
    zone = absl::FixedTimeZone(sign * ((hours * 3600) + (minutes * 60)));
  }
  absl::Time time = absl::FromCivil(civil, zone);
  if (const std::string point = match.str(7); !point.empty()) {
    std::string fraction = point.substr(1);
    fraction.resize(6, '0');
    time += absl::Microseconds(std::stoi(fraction));
  }
  return TimestampText(time);
}

absl::StatusOr<std::string> Formatted(FieldType type, std::string_view text,
                                      const DatetimeFormats& formats) {
  namespace functions = googlesql::functions;
  std::string out;
  switch (type) {
    case FieldType::kDate: {
      int32_t date = 0;
      GOOGLESQL_RETURN_IF_ERROR(
          functions::CastStringToDate(formats.date, text, CurrentDate(), &date));
      GOOGLESQL_RETURN_IF_ERROR(functions::ConvertDateToString(date, &out));
      return out;
    }
    case FieldType::kDatetime: {
      googlesql::DatetimeValue datetime;
      GOOGLESQL_RETURN_IF_ERROR(functions::CastStringToDatetime(
          formats.datetime, text, functions::kMicroseconds, CurrentDate(), &datetime));
      GOOGLESQL_RETURN_IF_ERROR(
          functions::ConvertDatetimeToString(datetime, functions::kMicroseconds, &out));
      return out;
    }
    case FieldType::kTime: {
      googlesql::TimeValue time;
      GOOGLESQL_RETURN_IF_ERROR(
          functions::CastStringToTime(formats.time, text, functions::kMicroseconds, &time));
      GOOGLESQL_RETURN_IF_ERROR(
          functions::ConvertTimeToString(time, functions::kMicroseconds, &out));
      return out;
    }
    default: {
      GOOGLESQL_ASSIGN_OR_RETURN(const absl::TimeZone zone, TimeZone(formats.time_zone));
      absl::Time time;
      GOOGLESQL_RETURN_IF_ERROR(
          functions::CastStringToTimestamp(formats.timestamp, text, zone, absl::Now(), &time));
      return TimestampText(time);
    }
  }
}

}  // namespace

absl::Status CheckDatetimeFormats(const DatetimeFormats& formats) {
  if (const auto zone = TimeZone(formats.time_zone); !zone.ok()) {
    return absl::InvalidArgumentError(zone.status().message());
  }
  for (const auto& [name, format, kind] : {
           std::tuple{"date_format", &formats.date, googlesql::TYPE_DATE},
           {"datetime_format", &formats.datetime, googlesql::TYPE_DATETIME},
           {"time_format", &formats.time, googlesql::TYPE_TIME},
           {"timestamp_format", &formats.timestamp, googlesql::TYPE_TIMESTAMP},
       }) {
    if (format->empty()) {
      continue;
    }
    if (const absl::Status valid =
            googlesql::functions::ValidateFormatStringForParsing(*format, kind);
        !valid.ok()) {
      return absl::InvalidArgumentError(
          absl::StrCat("Invalid format for option '", name, "': ", valid.message()));
    }
  }
  return absl::OkStatus();
}

absl::StatusOr<std::string> ExternalValue(FieldType type, std::string_view text,
                                          const DatetimeFormats& formats) {
  const std::string& format = type == FieldType::kDate        ? formats.date
                              : type == FieldType::kDatetime  ? formats.datetime
                              : type == FieldType::kTime      ? formats.time
                              : type == FieldType::kTimestamp ? formats.timestamp
                                                              : formats.date;
  const bool datetime = type == FieldType::kDate || type == FieldType::kDatetime ||
                        type == FieldType::kTime || type == FieldType::kTimestamp;
  if (datetime && !format.empty()) {
    return Formatted(type, text, formats);
  }
  if (type == FieldType::kTimestamp && !formats.time_zone.empty()) {
    GOOGLESQL_ASSIGN_OR_RETURN(const absl::TimeZone zone, TimeZone(formats.time_zone));
    return ZonedTimestamp(text, zone);
  }
  switch (type) {
    case FieldType::kDatetime:
      return Datetime(text);
    case FieldType::kBigNumeric:
      return BigNumeric(text);
    case FieldType::kJson:
      return Json(text);
    case FieldType::kGeography:
      return Geography(text);
    default:
      return std::string(text);
  }
}

}  // namespace bigquery_emulator_duckdb
