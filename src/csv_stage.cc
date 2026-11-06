#include "src/csv_stage.h"

#include <algorithm>
#include <cstddef>
#include <format>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/strings/escaping.h"
#include "src/api_error.h"
#include "src/csv_records.h"
#include "src/external_values.h"
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
    // BigQuery shortens a value of more than 20 characters.
    const std::string shown = value->size() > 20 ? value->substr(0, 17) + "..." : *value;
    message += std::format(" value: \"{}\"", absl::CEscape(shown));
  }
  return message;
}

std::vector<FileSource> StageCsvSources(const std::vector<FileSource>& sources,
                                        const CsvReadOptions& options,
                                        const std::vector<FieldSchema>& schema,
                                        TemporaryFiles& files, BadRecords* bad,
                                        const std::optional<std::set<std::string>>& read_columns) {
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
      bool external_conversion = false;
      if (bad != nullptr) {
        ++bad->records;
        bad->positions.push_back(position);
      }
      // The records end at such a record, so a read fails at it rather than skip the rest.
      if (record.error) {
        const size_t column = record.cells.size() - 1;
        Reject(*record.error, position, column < columns ? &schema.at(column) : nullptr, column,
               record.cells.back()->text);
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
          auto value = Value(column < found ? record.cells.at(column) : std::nullopt, field, column,
                             options, position);
          if (value && options.external_values &&
              (!read_columns || read_columns->contains(field.name))) {
            auto converted = ExternalValue(field.type, *value, options.formats);
            if (!converted.ok()) {
              external_conversion = true;
              const std::string detail(converted.status().message());
              if (absl::IsUnimplemented(converted.status())) {
                throw ApiError::Invalid(detail);
              }
              Reject(detail, position, &field, column, value);
            }
            value = *std::move(converted);
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
                .external_conversion = external_conversion,
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
