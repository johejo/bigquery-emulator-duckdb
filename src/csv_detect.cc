#include "src/csv_detect.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "src/api_error.h"
#include "src/csv_records.h"
#include "src/duckdb_sql.h"
#include "src/field_schema.h"
#include "src/file_read.h"
#include "src/schema_detect.h"
#include "src/temporary_files.h"

// The rules below follow BigQuery's answers in
// tests/e2e/goclient/testdata/unverified/load_csv_autodetect.api.answers.json, which
// third_party/bigquery_docs/loading-data-cloud-storage-csv.md describes only in outline. The
// types of values are in src/schema_detect.h.

namespace bigquery_emulator_duckdb {
namespace {

// A value of a record, absent when it is unquoted and empty or the record ends before it.
using Cell = std::optional<std::string>;
using Record = std::vector<Cell>;

// Splits `contents` into at most `limit` records at `delimiter` and line breaks, skipping blank
// lines. As without allowQuotedNewlines, a quoted value cannot hold a line break; `lenient` ends
// one that does not close at the end of its line.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): a separator and a count, named apart.
std::vector<Record> ParseRecords(std::string_view contents, char delimiter,
                                 size_t limit = std::numeric_limits<size_t>::max(),
                                 bool lenient = false) {
  std::vector<Record> records;
  for (CsvRecord& parsed : ParseCsvRecords(contents, {.delimiter = delimiter}, limit, lenient)) {
    std::ranges::transform(parsed.cells, std::back_inserter(records.emplace_back()),
                           [](std::optional<CsvCell>& cell) {
                             return cell ? Cell(std::move(cell->text)) : std::nullopt;
                           });
  }
  return records;
}

// The delimiter that splits the first record into the most fields: a comma, pipe or tab, which
// BigQuery detects. Of those that split it into as many, BigQuery picks a tab, then a pipe.
char DetectDelimiter(std::string_view contents) {
  constexpr std::array kCandidates = {',', '|', '\t'};
  std::array<size_t, kCandidates.size()> counts{};
  for (size_t i = 0; i < kCandidates.size(); ++i) {
    const std::vector<Record> first = ParseRecords(contents, kCandidates.at(i), 1, true);
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

using Kind = DetectedKind;

// The prefix of the names of columns of each type that auto-detection does not name from a
// header.

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

DetectedValue Classify(const Cell& cell) { return cell ? DetectValue(*cell) : DetectedValue{}; }

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
  CheckReservedFieldName(name);
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

DetectedCsv DetectCsv(const std::vector<FileSource>& sources, std::string_view field_delimiter,
                      std::optional<int> skip_leading_rows, TemporaryFiles& downloads, bool rows) {
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
  // Detection reads a quote that does not close leniently, but a read of the rows fails on it.
  const std::vector<Record> records =
      ParseRecords(contents, delimiter, std::numeric_limits<size_t>::max(), !rows);

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
  std::vector<std::vector<DetectedValue>> typed;
  for (size_t row = first; row < records.size(); ++row) {
    std::vector<DetectedValue>& values = typed.emplace_back();
    for (size_t column = 0; column < columns; ++column) {
      const Record& record = records.at(row);
      values.push_back(column < record.size() ? Classify(record.at(column)) : DetectedValue{});
    }
  }
  const auto kinds = [&](size_t begin) {
    std::vector<Kind> result(columns);
    for (size_t column = 0; column < columns; ++column) {
      result.at(column) = typed.at(begin - first).at(column).kind;
      for (size_t row = begin; row < records.size(); ++row) {
        result.at(column) = MergeKinds(result.at(column), typed.at(row - first).at(column).kind);
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
      header = header || MergeKinds(kind, rest.at(column)) != rest.at(column);
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
  detected.delimiter = delimiter;
  detected.skipped_rows = static_cast<int>(begin);
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

  if (!rows) {
    return detected;
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
