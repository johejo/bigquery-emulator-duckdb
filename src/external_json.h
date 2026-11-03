#pragma once

#include <vector>

#include "src/field_schema.h"
#include "src/file_read.h"
#include "src/file_read_options.h"
#include "src/temporary_files.h"

namespace bigquery_emulator_duckdb {

// Validates external JSON records, canonicalizes field names and fills absent nullable/repeated
// fields before the typed reader runs. Keeps source URIs for _FILE_NAME and error attribution.
// The values of an `autodetected` table convert as BigQuery reads them, such as a string that
// holds a number of a FLOAT64 field. A record BigQuery rejects throws ApiError::Invalid unless
// `bad` adds it and may skip it, which leaves it out.
std::vector<FileSource> StageExternalJson(const std::vector<FileSource>& sources,
                                          const std::vector<FieldSchema>& schema,
                                          TemporaryFiles& files, bool autodetected,
                                          BadRecords* bad = nullptr);

}  // namespace bigquery_emulator_duckdb
