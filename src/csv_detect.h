#pragma once

#include <optional>
#include <string_view>
#include <vector>

#include "src/field_schema.h"
#include "src/file_read.h"
#include "src/temporary_files.h"

namespace bigquery_emulator_duckdb {

// What schema auto-detection finds in CSV sources.
struct DetectedCsv {
  // The detected columns, or none when the sources have no rows.
  std::vector<FieldSchema> schema;
  // Comma-delimited copies of the sources' data rows without a header or skipped rows. Each value
  // is in a form FileReadQuery reads as its column's type; an unquoted empty value is NULL and a
  // quoted one an empty STRING.
  std::vector<FileSource> sources;
  // The delimiter of the sources and the number of their leading records that are not data.
  char delimiter = ',';
  int skipped_rows = 0;
};

// Detects the delimiter, header and column types of CSV `sources` as BigQuery does. An empty
// `field_delimiter` detects one; `skip_leading_rows` is absent when the job leaves it out.
// Without `rows`, the result has no sources, and a quote that does not close ends at the end of
// its line rather than failing, as in BigQuery's detection for external tables.
DetectedCsv DetectCsv(const std::vector<FileSource>& sources, std::string_view field_delimiter,
                      std::optional<int> skip_leading_rows, TemporaryFiles& downloads,
                      bool rows = true);

}  // namespace bigquery_emulator_duckdb
