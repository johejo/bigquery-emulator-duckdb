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
using CsvRecord = std::vector<std::optional<CsvCell>>;

// Splits `contents` into at most `limit` records, skipping blank lines. Throws ApiError::Invalid
// for a quote BigQuery does not close, unless `lenient`, as schema detection is, which ends the
// value at the end of its line.
std::vector<CsvRecord> ParseCsvRecords(std::string_view contents, const CsvDialect& dialect,
                                       size_t limit = std::numeric_limits<size_t>::max(),
                                       bool lenient = false);

}  // namespace bigquery_emulator_duckdb
