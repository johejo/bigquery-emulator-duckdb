#include "src/csv_stage.h"

#include <algorithm>
#include <cstddef>
#include <format>
#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <vector>

#include "absl/strings/escaping.h"
#include "src/api_error.h"
#include "src/csv_records.h"
#include "src/field_schema.h"
#include "src/file_read.h"
#include "src/file_read_options.h"
#include "src/temporary_files.h"

// The rules below follow BigQuery's answers in
// tests/e2e/scenarios/testdata/csv_options.api.answers.json, beyond what
// third_party/bigquery_docs/loading-data-cloud-storage-csv.md describes.

namespace bigquery_emulator_duckdb {
namespace {

// The name of `type` in standard SQL.
std::string_view SqlTypeName(FieldType type) {
  switch (type) {
    case FieldType::kInteger:
      return "INT64";
    case FieldType::kFloat:
      return "FLOAT64";
    case FieldType::kBoolean:
      return "BOOL";
    default:
      return FieldTypeName(type);
  }
}

// A record of `position` that BigQuery rejects, as CsvRecordError describes it.
[[noreturn]] void Reject(std::string_view detail, const RecordPosition& position,
                         const FieldSchema* field = nullptr, size_t index = 0,
                         const std::optional<std::string>& value = std::nullopt) {
  throw ApiError::Invalid(CsvRecordError(detail, position, field, index, value));
}

void AppendQuoted(std::string& out, std::string_view text) {
  out += '"';
  for (const char c : text) {
    out += c;
    if (c == '"') {
      out += c;
    }
  }
  out += '"';
}

// `text` with each control character other than a tab or line break, DEL included, a space, as
// BigQuery reads it unless it preserves them.
std::string Printable(std::string text, const CsvReadOptions& options) {
  if (!options.preserve_ascii_control_characters) {
    std::ranges::replace_if(
        text,
        [](char c) {
          return (c > '\0' && c < ' ' && c != '\t' && c != '\n' && c != '\r') || c == '\x7f';
        },
        ' ');
  }
  return text;
}

// The value of `cell`, of `field`, the column `index` of the record at `position`, or none for
// NULL. An absent cell is an unquoted empty value, as is a value a jagged row leaves out.
std::optional<std::string> Value(const std::optional<CsvCell>& cell, const FieldSchema& field,
                                 size_t index, const CsvReadOptions& options,
                                 const RecordPosition& position) {
  const std::string empty;
  const std::string& text = cell ? cell->text : empty;
  if (!options.preserve_ascii_control_characters && text.find('\0') != std::string::npos) {
    Reject("Bad character (ASCII 0) encountered.", position, &field, index, text);
  }
  if (!options.null_marker && !options.null_markers) {
    if (!cell || (cell->text.empty() && options.quoted_nulls)) {
      return std::nullopt;
    }
    return Printable(text, options);
  }
  // A NULL marker matches quoted values too, and replaces the empty value as NULL.
  const std::vector<std::string> markers =
      options.null_markers.value_or(std::vector<std::string>{options.null_marker.value_or("")});
  if (std::ranges::find(markers, text) != markers.end()) {
    return std::nullopt;
  }
  if (text.empty() && field.type != FieldType::kString && field.type != FieldType::kBytes) {
    Reject(std::format("Null marker is set to '{}' but empty value found for field {} that is "
                       "of type different than STRING or BYTES.",
                       markers.front(), field.name),
           position, &field, index, text);
  }
  return Printable(text, options);
}

}  // namespace

std::string CsvRecordError(std::string_view detail, const RecordPosition& position,
                           const FieldSchema* field, size_t index,
                           const std::optional<std::string>& value) {
  std::string message = std::format(
      "Error while reading data, error message: {}; line_number: {} "
      "byte_offset_to_start_of_line: {}",
      detail, position.line, position.offset);
  if (field != nullptr) {
    message += std::format(" column_index: {} column_name: \"{}\" column_type: {}", index,
                           absl::CEscape(field->name), SqlTypeName(field->type));
  }
  if (field != nullptr && value) {
    message += std::format(" value: \"{}\"", absl::CEscape(*value));
  }
  return message;
}

std::vector<FileSource> StageCsvSources(const std::vector<FileSource>& sources,
                                        const CsvReadOptions& options,
                                        const std::vector<FieldSchema>& schema,
                                        TemporaryFiles& files, BadRecords* bad) {
  const CsvDialect dialect{
      .delimiter = options.field_delimiter.front(),
      .quote = options.quote,
      .allow_quoted_newlines = options.allow_quoted_newlines,
  };
  const size_t columns = schema.size();
  std::vector<FileSource> staged;
  for (const FileSource& source : sources) {
    const std::vector<CsvRecord> records = ParseCsvRecords(ReadMaybeGzip(source.path), dialect);
    std::string rewritten;
    for (size_t row = std::min<size_t>(options.skip_leading_rows, records.size());
         row < records.size(); ++row) {
      const CsvRecord& record = records.at(row);
      const RecordPosition position{.line = record.line, .offset = record.offset};
      const size_t found = record.cells.size();
      const bool too_many = found > columns && !options.ignore_unknown_values;
      std::string line;
      if (bad != nullptr) {
        ++bad->records;
        bad->positions.push_back(position);
      }
      try {
        if (too_many) {
          Reject(std::format("Too many values in line. Found {} column(s) when expecting {}.",
                             found, columns),
                 position);
        }
        for (size_t column = 0; column < columns; ++column) {
          const FieldSchema& field = schema.at(column);
          if (column > 0) {
            line += ',';
          }
          if (column >= found && !options.allow_jagged_rows) {
            Reject(std::format("CSV table references column position {}, but line contains only "
                               "{} columns.",
                               column, found),
                   position, &field, column);
          }
          const auto value = Value(column < found ? record.cells.at(column) : std::nullopt, field,
                                   column, options, position);
          if (value && options.canonical_datetimes && field.type == FieldType::kDatetime) {
            static const std::regex datetime(
                R"([0-9]{4}-[0-9]{2}-[0-9]{2} ([01][0-9]|2[0-3]):[0-5][0-9]:[0-5][0-9](\.[0-9]{1,6})?)");
            if (!std::regex_match(*value, datetime)) {
              throw ApiError::Unsupported("CSV external representation for " + field.name);
            }
          }
          if (value) {
            AppendQuoted(line, *value);
          } else if (column >= found && field.mode == FieldMode::kRequired) {
            Reject("Required column value is missing.", position, &field, column);
          }
        }
      } catch (const ApiError& error) {
        if (bad == nullptr || std::string_view(error.what()).starts_with(kUnsupported) ||
            !bad->Skip({
                .number = bad->records,
                .uri = source.uri,
                .message = error.what(),
                .row = !too_many,
            })) {
          throw;
        }
        continue;
      }
      if (bad != nullptr && options.numbered_records) {
        rewritten += std::to_string(bad->records) + ',';
      }
      rewritten += line + '\n';
    }
    staged.push_back({.uri = source.uri, .path = files.Write(rewritten)});
  }
  return staged;
}

}  // namespace bigquery_emulator_duckdb
