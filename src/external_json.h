#pragma once

#include <vector>

#include "src/field_schema.h"
#include "src/file_read.h"
#include "src/temporary_files.h"

namespace bigquery_emulator_duckdb {

// Validates external JSON records, canonicalizes field names and fills absent nullable/repeated
// fields before the typed reader runs. Keeps source URIs for _FILE_NAME and error attribution.
std::vector<FileSource> StageExternalJson(const std::vector<FileSource>& sources,
                                          const std::vector<FieldSchema>& schema,
                                          TemporaryFiles& files);

}  // namespace bigquery_emulator_duckdb
