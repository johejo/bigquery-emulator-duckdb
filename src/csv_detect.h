#pragma once

#include <optional>
#include <string_view>
#include <vector>

#include "src/field_schema.h"
#include "src/file_read.h"
#include "src/temporary_files.h"

namespace bigquery_emulator_duckdb {

// What a load with schema auto-detection reads from CSV sources.
struct DetectedCsv {
  // The detected columns, or none when the sources have no rows.
  std::vector<FieldSchema> schema;
  // Comma-delimited copies of the sources' data rows without a header or skipped rows. Each value
  // is in a form FileReadQuery reads as its column's type; an unquoted empty value is NULL and a
  // quoted one an empty STRING.
  std::vector<FileSource> sources;
};

// Validates CSV quoting, including the default prohibition on quoted line breaks.
void ValidateCsvRecords(const FileSource& source, char delimiter);

// Detects the delimiter, header and column types of CSV `sources` as BigQuery does. An empty
// `field_delimiter` detects one; `skip_leading_rows` is absent when the job leaves it out.
DetectedCsv DetectCsv(const std::vector<FileSource>& sources, std::string_view field_delimiter,
                      std::optional<int> skip_leading_rows, TemporaryFiles& downloads);

}  // namespace bigquery_emulator_duckdb
