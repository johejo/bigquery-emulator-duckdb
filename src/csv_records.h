#pragma once

#include <cstddef>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bigquery_emulator_duckdb {

// How a CSV source separates records and values.
struct CsvDialect {
  char delimiter = ',';
  // None when values are never quoted.
  std::optional<char> quote = '"';
  bool allow_quoted_newlines = false;
};

// A value of a record, absent when it is unquoted and empty or the record ends before it.
struct CsvCell {
  std::string text;
  bool quoted = false;
};
struct CsvRecord {
  std::vector<std::optional<CsvCell>> cells;
  // The line it starts on, counting from 1.
  size_t line = 0;
};

// Splits `contents` into at most `limit` records, skipping blank lines. Throws ApiError::Invalid
// for a quote BigQuery does not close or data after a closing quote, unless `lenient`, as schema
// detection is, which ends such a value at the end of its line or keeps the data.
std::vector<CsvRecord> ParseCsvRecords(std::string_view contents, const CsvDialect& dialect,
                                       size_t limit = std::numeric_limits<size_t>::max(),
                                       bool lenient = false);

// The quote character of a CSV quote option, none for an empty one. Throws ApiError::Invalid for
// more than one character, as BigQuery does, and ApiError::Unsupported for one that is not ASCII
// or is the field delimiter or a line break.
std::optional<char> ParseCsvQuote(const std::string& quote, const std::string& field_delimiter);

// Throws ApiError::Invalid when both nullMarker and nullMarkers are set, as BigQuery does.
void CheckCsvNullMarkers(bool null_marker, bool null_markers);

}  // namespace bigquery_emulator_duckdb
