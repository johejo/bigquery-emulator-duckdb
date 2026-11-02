#include "src/schema_detect.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "absl/time/time.h"
#include "googlesql/public/functions/date_time_util.h"
#include "src/api_error.h"
#include "src/duckdb_sql.h"
#include "src/field_schema.h"

// The rules below follow BigQuery's answers in
// tests/e2e/goclient/testdata/unverified/load_csv_autodetect.api.answers.json and
// tests/e2e/scenarios/testdata/external_table_json_autodetect.api.answers.json, which
// third_party/bigquery_docs/schema-detect.md describes only in outline.

namespace bigquery_emulator_duckdb {
namespace {

std::string_view Trim(std::string_view text) {
  const size_t begin = text.find_first_not_of(" \t");
  if (begin == std::string_view::npos) {
    return {};
  }
  return text.substr(begin, text.find_last_not_of(" \t") - begin + 1);
}

bool IsDigit(char c) { return c >= '0' && c <= '9'; }

bool AllDigits(std::string_view text) {
  return !text.empty() && std::ranges::all_of(text, IsDigit);
}

// The digits of an integer with an optional sign and thousands separators, such as -1,000,
// without the separators or a plus sign.
std::optional<std::string> IntegerDigits(std::string_view text) {
  std::string digits;
  if (!text.empty() && (text.front() == '+' || text.front() == '-')) {
    if (text.front() == '-') {
      digits += '-';
    }
    text.remove_prefix(1);
  }
  const size_t group = std::min(text.find(','), text.size());
  if (!AllDigits(text.substr(0, group)) || (group < text.size() && group > 3)) {
    return std::nullopt;
  }
  digits.append(text.substr(0, group));
  for (text.remove_prefix(group); !text.empty(); text.remove_prefix(4)) {
    if (text.size() < 4 || text.front() != ',' || !AllDigits(text.substr(1, 3))) {
      return std::nullopt;
    }
    digits.append(text.substr(1, 3));
  }
  return digits;
}

// `text` as strtod reads it, which takes hexadecimal integers, infinities and NaN as BigQuery
// does.
std::optional<double> ParseFloat(std::string_view text) {
  const std::string copy(text);
  char* end = nullptr;
  const double value = std::strtod(copy.c_str(), &end);
  if (copy.empty() || end != copy.c_str() + copy.size()) {
    return std::nullopt;
  }
  return value;
}

std::string FloatText(double value) {
  if (std::isnan(value)) {
    return "NaN";
  }
  if (std::isinf(value)) {
    return value > 0 ? "Infinity" : "-Infinity";
  }
  std::array<char, 32> buffer{};
  const auto result = std::to_chars(buffer.begin(), buffer.end(), value);
  return {buffer.begin(), result.ptr};
}

// Reads `min` to `max` digits of `text` at `pos` into `value`, returning the position after them.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): a position and the bounds of a count.
std::optional<size_t> ReadNumber(std::string_view text, size_t pos, size_t min, size_t max,
                                 int& value) {
  value = 0;
  size_t end = pos;
  for (; end < text.size() && end - pos < max && IsDigit(text.at(end)); ++end) {
    value = (value * 10) + (text.at(end) - '0');
  }
  return end - pos >= min ? std::optional(end) : std::nullopt;
}

struct Date {
  int year = 0;
  int month = 0;
  int day = 0;
};

// Reads a date of `text` at its start, YYYY-M-D or YYYY/M/D, returning the position after it.
std::optional<size_t> ReadDate(std::string_view text, Date& date) {
  std::optional<size_t> pos = ReadNumber(text, 0, 4, 4, date.year);
  if (!pos || *pos >= text.size() || (text.at(*pos) != '-' && text.at(*pos) != '/')) {
    return std::nullopt;
  }
  const char separator = text.at(*pos);
  pos = ReadNumber(text, *pos + 1, 1, 2, date.month);
  if (!pos || *pos >= text.size() || text.at(*pos) != separator) {
    return std::nullopt;
  }
  pos = ReadNumber(text, *pos + 1, 1, 2, date.day);
  if (!pos || date.year < 1 || date.month < 1 || date.month > 12 || date.day < 1) {
    return std::nullopt;
  }
  constexpr std::array kDays = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  const bool leap = date.year % 4 == 0 && (date.year % 100 != 0 || date.year % 400 == 0);
  const int days = kDays.at(date.month - 1) + (date.month == 2 && leap ? 1 : 0);
  return date.day <= days ? pos : std::nullopt;
}

// Reads a time of `text` at `pos`, HH:MM:SS with up to nine fractional digits, or HH:MM unless
// `seconds` is required, into `time` as HH:MM:SS with at most six. Returns the position after it.
std::optional<size_t> ReadTime(std::string_view text, size_t pos, bool seconds, std::string& time) {
  int hour = 0;
  int minute = 0;
  int second = 0;
  std::optional<size_t> end = ReadNumber(text, pos, 2, 2, hour);
  if (!end || *end >= text.size() || text.at(*end) != ':') {
    return std::nullopt;
  }
  end = ReadNumber(text, *end + 1, 2, 2, minute);
  if (!end) {
    return std::nullopt;
  }
  std::string fraction;
  if (*end < text.size() && text.at(*end) == ':') {
    end = ReadNumber(text, *end + 1, 2, 2, second);
    if (!end) {
      return std::nullopt;
    }
    if (*end < text.size() && text.at(*end) == '.') {
      size_t digits = *end + 1;
      for (; digits < text.size() && digits - *end <= 9 && IsDigit(text.at(digits)); ++digits) {
      }
      if (digits == *end + 1) {
        return std::nullopt;
      }
      fraction = "." + std::string(text.substr(*end + 1, std::min<size_t>(digits - *end - 1, 6)));
      end = digits;
    }
  } else if (seconds) {
    return std::nullopt;
  }
  if (hour > 23 || minute > 59 || second > 59) {
    return std::nullopt;
  }
  time = std::format("{:02}:{:02}:{:02}{}", hour, minute, second, fraction);
  return end;
}

// The UTC text of a TIMESTAMP at `date` and `time` in `zone`, which is empty, Z, an offset or a
// time zone name after a space, as GoogleSQL reads it.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the parts of a timestamp, in order.
std::optional<std::string> TimestampText(const Date& date, std::string_view time,
                                         std::string_view zone) {
  std::string text = std::format("{:04}-{:02}-{:02} {}", date.year, date.month, date.day, time);
  // GoogleSQL takes no space before an offset, which a CSV value can have, as in
  // 2018-08-19T07:11:35.220 -05:00.
  if (const std::string_view offset = Trim(zone);
      !offset.empty() && (offset.front() == '+' || offset.front() == '-')) {
    zone = offset;
  }
  text += zone == "Z" || zone == "z" ? "+00" : zone;
  absl::Time timestamp;
  if (!googlesql::functions::ConvertStringToTimestamp(text, absl::UTCTimeZone(),
                                                      googlesql::functions::kMicroseconds,
                                                      /*allow_tz_in_str=*/true, &timestamp)
           .ok()) {
    return std::nullopt;
  }
  return absl::FormatTime("%Y-%m-%d %H:%M:%E6S+00", timestamp, absl::UTCTimeZone());
}

}  // namespace

DetectedValue DetectValue(std::string_view input) {
  const std::string_view text = Trim(input);
  if (text.empty()) {
    return {};
  }
  const std::string lower = ToLowerAscii(text);
  for (const std::string_view value : {
           "true",
           "t",
           "yes",
           "y",
       }) {
    if (lower == value) {
      return {.kind = DetectedKind::kBoolean, .text = "true"};
    }
  }
  for (const std::string_view value : {
           "false",
           "f",
           "no",
           "n",
       }) {
    if (lower == value) {
      return {.kind = DetectedKind::kBoolean, .text = "false"};
    }
  }
  // An integer out of INT64's range is a FLOAT64.
  if (const std::optional<std::string> digits = IntegerDigits(text)) {
    int64_t value = 0;
    const auto result = std::from_chars(digits->data(), digits->data() + digits->size(), value);
    if (result.ec == std::errc()) {
      return {.kind = DetectedKind::kInteger, .text = std::to_string(value)};
    }
    return {.kind = DetectedKind::kFloat, .text = FloatText(std::strtod(digits->c_str(), nullptr))};
  }
  if (const std::optional<double> value = ParseFloat(text)) {
    return {.kind = DetectedKind::kFloat, .text = FloatText(*value)};
  }
  // A date with a time is a TIMESTAMP, never a DATETIME.
  Date date;
  if (const std::optional<size_t> end = ReadDate(text, date)) {
    if (*end == text.size()) {
      return {
          .kind = DetectedKind::kDate,
          .text = std::format("{:04}-{:02}-{:02}", date.year, date.month, date.day),
      };
    }
    if (const char separator = text.at(*end);
        separator == ' ' || separator == 'T' || separator == 't') {
      std::string time;
      if (const std::optional<size_t> zone = ReadTime(text, *end + 1, /*seconds=*/false, time)) {
        if (std::optional<std::string> timestamp = TimestampText(date, time, text.substr(*zone))) {
          return {.kind = DetectedKind::kTimestamp, .text = *std::move(timestamp)};
        }
      }
    }
    return {};
  }
  std::string time;
  if (ReadTime(text, 0, /*seconds=*/true, time) == text.size()) {
    return {.kind = DetectedKind::kTime, .text = time};
  }
  return {};
}

DetectedKind MergeKinds(DetectedKind a, DetectedKind b) {
  if (a == b) {
    return a;
  }
  const auto numeric = [](DetectedKind kind) {
    return kind == DetectedKind::kInteger || kind == DetectedKind::kFloat;
  };
  return numeric(a) && numeric(b) ? DetectedKind::kFloat : DetectedKind::kString;
}

FieldType KindType(DetectedKind kind) {
  switch (kind) {
    case DetectedKind::kInteger:
      return FieldType::kInteger;
    case DetectedKind::kFloat:
      return FieldType::kFloat;
    case DetectedKind::kBoolean:
      return FieldType::kBoolean;
    case DetectedKind::kDate:
      return FieldType::kDate;
    case DetectedKind::kTime:
      return FieldType::kTime;
    case DetectedKind::kTimestamp:
      return FieldType::kTimestamp;
    case DetectedKind::kString:
      break;
  }
  return FieldType::kString;
}

void CheckDetectedFieldName(const std::string& name) {
  if (name.empty() || name.size() > 300 ||
      (std::isalpha(static_cast<unsigned char>(name.front())) == 0 && name.front() != '_') ||
      !std::ranges::all_of(name,
                           [](unsigned char c) { return std::isalnum(c) != 0 || c == '_'; })) {
    throw ApiError::Invalid(std::format(
        "Invalid field name \"{}\". Fields must contain only letters, numbers, and underscores, "
        "start with a letter or underscore, and be at most 300 characters long.",
        name));
  }
}

void CheckReservedFieldName(const std::string& name) {
  const std::string upper = ToUpperAscii(name);
  for (const std::string_view prefix : {
           "_PARTITION",
           "_TABLE_",
           "_FILE_",
           "_ROW_TIMESTAMP",
           "__ROOT__",
           "_COLIDENTIFIER",
           "__DREMEL_PK_MERGED_STRUCT_",
           "_CHANGE_SEQUENCE_NUMBER",
           "_CHANGE_TYPE",
           "_CHANGE_TIMESTAMP",
       }) {
    if (upper.starts_with(prefix)) {
      throw ApiError::Invalid(std::format(
          "Invalid field name \"{}\". Field names are not allowed to start with the "
          "(case-insensitive) prefixes _PARTITION, _TABLE_, _FILE_, _ROW_TIMESTAMP, __ROOT__, "
          "_COLIDENTIFIER, and __DREMEL_PK_MERGED_STRUCT_",
          name));
    }
  }
}

}  // namespace bigquery_emulator_duckdb
