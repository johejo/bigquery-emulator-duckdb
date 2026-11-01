#include "src/csv_stage.h"

#include <algorithm>
#include <cstddef>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

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

[[noreturn]] void Reject(std::string_view detail, size_t line) {
  throw ApiError::Invalid(
      std::format("Error while reading data, error message: {}; line_number: {}", detail, line));
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

// `text` with each control character other than a tab or line break a space, as BigQuery reads
// it unless it preserves them.
std::string Printable(std::string text, const CsvReadOptions& options) {
  if (!options.preserve_ascii_control_characters) {
    std::ranges::replace_if(
        text, [](char c) { return c > '\0' && c < ' ' && c != '\t' && c != '\n' && c != '\r'; },
        ' ');
  }
  return text;
}

// The value of `cell`, of `field`, in record `line`, or none for NULL. An absent cell is an
// unquoted empty value, as is a value a jagged row leaves out.
std::optional<std::string> Value(const std::optional<CsvCell>& cell, const FieldSchema& field,
                                 const CsvReadOptions& options, size_t line) {
  const std::string empty;
  const std::string& text = cell ? cell->text : empty;
  if (!options.preserve_ascii_control_characters && text.find('\0') != std::string::npos) {
    Reject("Bad character (ASCII 0) encountered.", line);
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
    if (options.null_markers) {
      throw ApiError::Unsupported("an empty CSV value of a non-STRING column with null_markers");
    }
    Reject(std::format("Null marker is set to '{}' but empty value found for field {} that is "
                       "of type different than STRING or BYTES.",
                       *options.null_marker, field.name),
           line);
  }
  return Printable(text, options);
}

}  // namespace

std::vector<FileSource> StageCsvSources(const std::vector<FileSource>& sources,
                                        const CsvReadOptions& options,
                                        const std::vector<FieldSchema>& schema,
                                        TemporaryFiles& files) {
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
      const size_t found = record.cells.size();
      if (found > columns && !options.ignore_unknown_values) {
        Reject(std::format("Too many values in line. Found {} column(s) when expecting {}.", found,
                           columns),
               record.line);
      }
      for (size_t column = 0; column < columns; ++column) {
        const FieldSchema& field = schema.at(column);
        if (column > 0) {
          rewritten += ',';
        }
        if (column >= found && !options.allow_jagged_rows) {
          Reject(std::format("CSV table references column position {}, but line contains only {} "
                             "columns.",
                             column, found),
                 record.line);
        }
        const auto value = Value(column < found ? record.cells.at(column) : std::nullopt, field,
                                 options, record.line);
        if (value) {
          AppendQuoted(rewritten, *value);
        } else if (column >= found && field.mode == FieldMode::kRequired) {
          Reject("Required column value is missing.", record.line);
        }
      }
      rewritten += '\n';
    }
    staged.push_back({.uri = source.uri, .path = files.Write(rewritten)});
  }
  return staged;
}

}  // namespace bigquery_emulator_duckdb
