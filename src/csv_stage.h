#pragma once

#include <vector>

#include "src/field_schema.h"
#include "src/file_read.h"
#include "src/file_read_options.h"
#include "src/temporary_files.h"

namespace bigquery_emulator_duckdb {

// Reads CSV `sources` with `options` as BigQuery does for `schema`, and writes copies that
// FileReadQuery reads: comma-delimited records of every column, each value quoted and each NULL
// an unquoted empty value. Throws ApiError::Invalid for a record BigQuery rejects.
std::vector<FileSource> StageCsvSources(const std::vector<FileSource>& sources,
                                        const CsvReadOptions& options,
                                        const std::vector<FieldSchema>& schema,
                                        TemporaryFiles& files);

}  // namespace bigquery_emulator_duckdb
