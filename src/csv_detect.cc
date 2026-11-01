#include "src/csv_detect.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "absl/time/time.h"
#include "googlesql/public/functions/date_time_util.h"
#include "src/api_error.h"
#include "src/duckdb_sql.h"
#include "src/field_schema.h"
#include "src/file_read.h"
#include "src/temporary_files.h"

// The rules below follow BigQuery's answers in
// tests/e2e/goclient/testdata/unverified/load_csv_autodetect.api.answers.json, which
// third_party/bigquery_docs/loading-data-cloud-storage-csv.md describes only in outline.

namespace bigquery_emulator_duckdb {
namespace {

// A value of a record, absent when it is unquoted and empty or the record ends before it.
using Cell = std::optional<std::string>;
using Record = std::vector<Cell>;

// Splits `contents` into at most `limit` records at `delimiter` and line breaks, skipping blank
// lines. As without allowQuotedNewlines, a quoted value cannot hold a line break.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): a separator and a count, named apart.
std::vector<Record> ParseRecords(std::string_view contents, char delimiter,
                                 size_t limit = std::numeric_limits<size_t>::max()) {
  std::vector<Record> records;
  size_t line = 1;
  size_t i = 0;
  const auto line_end = [&] {
    return i >= contents.size() || contents.at(i) == '\n' || contents.at(i) == '\r';
  };
  while (i < contents.size() && records.size() < limit) {
    Record record;
    while (true) {
      std::string value;
      bool quoted = false;
      if (i < contents.size() && contents.at(i) == '"') {
        quoted = true;
        for (++i;; ++i) {
          if (line_end()) {
            throw ApiError::Invalid(
                std::format("Error while reading data, error message: Missing close quote "
                            "character (\").; line_number: {}",
                            line));
          }
          if (contents.at(i) == '"') {
            if (i + 1 >= contents.size() || contents.at(i + 1) != '"') {
              ++i;
              break;
            }
            ++i;
          }
          value += contents.at(i);
        }
      }
      for (; !line_end() && contents.at(i) != delimiter; ++i) {
        value += contents.at(i);
      }
      record.push_back(quoted || !value.empty() ? Cell(std::move(value)) : std::nullopt);
      if (line_end()) {
        break;
      }
      ++i;
    }
    if (i < contents.size() && contents.at(i) == '\r') {
      ++i;
    }
    if (i < contents.size() && contents.at(i) == '\n') {
      ++i;
    }
    ++line;
    if (record.size() > 1 || record.front().has_value()) {
      records.push_back(std::move(record));
    }
  }
  return records;
}

// The delimiter that splits the first record into the most fields: a comma, pipe or tab, which
// BigQuery detects. Of those that split it into as many, BigQuery picks a tab, then a pipe.
char DetectDelimiter(std::string_view contents) {
  constexpr std::array kCandidates = {',', '|', '\t'};
  std::array<size_t, kCandidates.size()> counts{};
  for (size_t i = 0; i < kCandidates.size(); ++i) {
    const std::vector<Record> first = ParseRecords(contents, kCandidates.at(i), 1);
    counts.at(i) = first.empty() ? 0 : first.front().size();
  }
  const size_t most = std::ranges::max(counts);
  if (most <= 1) {
    return ',';
  }
  if (counts.at(2) == most) {
    return '\t';
  }
  return counts.at(1) == most ? '|' : ',';
}

// The types auto-detection gives values, with the prefix of the names of columns it does not
// name from a header.
enum class Kind : uint8_t { kString, kInteger, kFloat, kBoolean, kDate, kTime, kTimestamp };

struct KindInfo {
  Kind kind;
  FieldType type;
  std::string_view prefix;
};
constexpr std::array kKinds = {
    KindInfo{.kind = Kind::kString, .type = FieldType::kString, .prefix = "string"},
    KindInfo{.kind = Kind::kInteger, .type = FieldType::kInteger, .prefix = "int64"},
    KindInfo{.kind = Kind::kFloat, .type = FieldType::kFloat, .prefix = "double"},
    KindInfo{.kind = Kind::kBoolean, .type = FieldType::kBoolean, .prefix = "bool"},
    KindInfo{.kind = Kind::kDate, .type = FieldType::kDate, .prefix = "date"},
    KindInfo{.kind = Kind::kTime, .type = FieldType::kTime, .prefix = "time"},
    KindInfo{.kind = Kind::kTimestamp, .type = FieldType::kTimestamp, .prefix = "timestamp"},
};

const KindInfo& Info(Kind kind) { return *std::ranges::find(kKinds, kind, &KindInfo::kind); }

// The type of a column holding values of types `a` and `b`. An empty value is a STRING, so a
// column with one is a STRING column.
Kind Merge(Kind a, Kind b) {
  if (a == b) {
    return a;
  }
  const auto numeric = [](Kind kind) { return kind == Kind::kInteger || kind == Kind::kFloat; };
  return numeric(a) && numeric(b) ? Kind::kFloat : Kind::kString;
}

// A value's detected type, and its text in a form FileReadQuery reads as that type.
struct Typed {
  Kind kind = Kind::kString;
  std::string text;
};

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

Typed Classify(const Cell& cell) {
  const std::string_view text = cell ? Trim(*cell) : std::string_view();
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
      return {.kind = Kind::kBoolean, .text = "true"};
    }
  }
  for (const std::string_view value : {
           "false",
           "f",
           "no",
           "n",
       }) {
    if (lower == value) {
      return {.kind = Kind::kBoolean, .text = "false"};
    }
  }
  // An integer out of INT64's range is a FLOAT64.
  if (const std::optional<std::string> digits = IntegerDigits(text)) {
    int64_t value = 0;
    const auto result = std::from_chars(digits->data(), digits->data() + digits->size(), value);
    if (result.ec == std::errc()) {
      return {.kind = Kind::kInteger, .text = std::to_string(value)};
    }
    return {.kind = Kind::kFloat, .text = FloatText(std::strtod(digits->c_str(), nullptr))};
  }
  if (const std::optional<double> value = ParseFloat(text)) {
    return {.kind = Kind::kFloat, .text = FloatText(*value)};
  }
  // A date with a time is a TIMESTAMP, never a DATETIME.
  Date date;
  if (const std::optional<size_t> end = ReadDate(text, date)) {
    if (*end == text.size()) {
      return {
          .kind = Kind::kDate,
          .text = std::format("{:04}-{:02}-{:02}", date.year, date.month, date.day),
      };
    }
    if (const char separator = text.at(*end);
        separator == ' ' || separator == 'T' || separator == 't') {
      std::string time;
      if (const std::optional<size_t> zone = ReadTime(text, *end + 1, /*seconds=*/false, time)) {
        if (std::optional<std::string> timestamp = TimestampText(date, time, text.substr(*zone))) {
          return {.kind = Kind::kTimestamp, .text = *std::move(timestamp)};
        }
      }
    }
    return {};
  }
  std::string time;
  if (ReadTime(text, 0, /*seconds=*/true, time) == text.size()) {
    return {.kind = Kind::kTime, .text = time};
  }
  return {};
}

// Checks the characters of a column name taken from a header as BigQuery does with its default
// character map: of the characters flexible column names do not support
// (third_party/bigquery_docs/schemas.md), it checks every name before other rules.
// BigQuery rejects a tab too, but keeps spaces, even leading and trailing ones.
void CheckHeaderCharacters(const std::string& name) {
  if (name.find_first_of("!\"$()*,./;?@[\\]^`{}~\t") != std::string::npos) {
    throw ApiError::InvalidQuery(
        std::format("Field name '{}' is not supported by the current character map. Please "
                    "change your field name or use character map V2 to let the system modify "
                    "the field names.",
                    name));
  }
}

// Checks the other rules for a column name taken from a header.
void CheckHeaderName(const std::string& name) {
  // How BigQuery treats other whitespace is not known.
  if (name.find_first_of("\f\v") != std::string::npos) {
    throw ApiError::Unsupported("detecting a CSV header name with a form feed or vertical tab");
  }
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

std::string Quoted(std::string_view text) {
  std::string quoted = "\"";
  for (const char c : text) {
    quoted += c;
    if (c == '"') {
      quoted += c;
    }
  }
  return quoted + '"';
}

}  // namespace

void ValidateCsvRecords(const FileSource& source, char delimiter) {
  ParseRecords(ReadMaybeGzip(source.path), delimiter);
}

DetectedCsv DetectCsv(const std::vector<FileSource>& sources, std::string_view field_delimiter,
                      std::optional<int> skip_leading_rows, TemporaryFiles& downloads) {
  // BigQuery samples one of several files, and whether it then finds a header in each is not
  // known.
  if (sources.size() > 1) {
    throw ApiError::Unsupported("schema auto-detection over several CSV files");
  }
  if (sources.empty()) {
    return {};
  }
  if (field_delimiter.size() > 1) {
    throw ApiError::Unsupported("schema auto-detection with the field delimiter " +
                                std::string(field_delimiter));
  }
  const std::string contents = ReadMaybeGzip(sources.front().path);
  const char delimiter =
      field_delimiter.empty() ? DetectDelimiter(contents) : field_delimiter.front();
  const std::vector<Record> records = ParseRecords(contents, delimiter);

  // Left out, skipLeadingRows looks for a header in the first row, which is otherwise data;
  // N > 0 skips N - 1 rows and looks for one in row N, which it skips either way; 0 reads every
  // row as data.
  std::optional<size_t> candidate;
  size_t data = 0;
  if (!skip_leading_rows.has_value() || *skip_leading_rows > 0) {
    const size_t row = skip_leading_rows.has_value() ? *skip_leading_rows - 1 : 0;
    if (row < records.size()) {
      candidate = row;
    }
    data = std::min(row + 1, records.size());
  }
  const size_t first = candidate.value_or(data);
  size_t columns = 0;
  for (size_t row = first; row < records.size(); ++row) {
    columns = std::max(columns, records.at(row).size());
  }
  if (columns == 0) {
    return {};
  }
  std::vector<std::vector<Typed>> typed;
  for (size_t row = first; row < records.size(); ++row) {
    std::vector<Typed>& values = typed.emplace_back();
    for (size_t column = 0; column < columns; ++column) {
      const Record& record = records.at(row);
      values.push_back(column < record.size() ? Classify(record.at(column)) : Typed{});
    }
  }
  const auto kinds = [&](size_t begin) {
    std::vector<Kind> result(columns);
    for (size_t column = 0; column < columns; ++column) {
      result.at(column) = typed.at(begin - first).at(column).kind;
      for (size_t row = begin; row < records.size(); ++row) {
        result.at(column) = Merge(result.at(column), typed.at(row - first).at(column).kind);
      }
    }
    return result;
  };

  // The candidate is a header when one of its values does not fit the type of the rest of its
  // column, so a header of strings over STRING columns is data.
  bool header = false;
  if (candidate.has_value() && data < records.size()) {
    const std::vector<Kind> rest = kinds(data);
    for (size_t column = 0; column < columns; ++column) {
      const Kind kind = typed.front().at(column).kind;
      header = header || Merge(kind, rest.at(column)) != rest.at(column);
    }
  }
  const size_t begin =
      candidate.has_value() && !header && !skip_leading_rows.has_value() ? *candidate : data;
  // With no data rows, the candidate still gives the types.
  const std::vector<Kind> types = kinds(begin < records.size() ? begin : first);

  std::vector<Cell> header_names(columns);
  if (header) {
    const Record& record = records.at(first);
    for (size_t column = 0; column < record.size(); ++column) {
      const Cell& cell = record.at(column);
      header_names.at(column) = cell;
      if (cell.has_value()) {
        CheckHeaderCharacters(*cell);
      }
    }
  }
  DetectedCsv detected;
  std::vector<std::string> names;
  for (size_t column = 0; column < columns; ++column) {
    std::string name;
    if (const Cell& cell = header_names.at(column); cell.has_value()) {
      name = *cell;
      CheckHeaderName(name);
    } else {
      name = std::format("{}_field_{}", Info(types.at(column)).prefix, column);
    }
    const std::string lower = ToLowerAscii(name);
    if (std::ranges::find(names, lower) != names.end()) {
      throw ApiError::InvalidQuery(std::format("Duplicated field name '{}'", name));
    }
    names.push_back(lower);
    detected.schema.push_back({.name = name, .type = Info(types.at(column)).type});
  }

  std::string rewritten;
  for (size_t row = begin; row < records.size(); ++row) {
    const Record& record = records.at(row);
    for (size_t column = 0; column < columns; ++column) {
      if (column > 0) {
        rewritten += ',';
      }
      const Cell absent;
      const Cell& cell = column < record.size() ? record.at(column) : absent;
      if (cell.has_value()) {
        rewritten += Quoted(
            types.at(column) == Kind::kString ? *cell : typed.at(row - first).at(column).text);
      }
    }
    rewritten += '\n';
  }
  detected.sources.push_back({.uri = sources.front().uri, .path = downloads.Write(rewritten)});
  return detected;
}

}  // namespace bigquery_emulator_duckdb
