#pragma once

#include <string>
#include <vector>

#include "src/field_schema.h"
#include "src/gcs.h"
#include "src/job_options.h"
#include "src/parquet_metadata.h"
#include "src/temporary_files.h"

namespace bigquery_emulator_duckdb {

// Stages the files that `uris` name for
// DuckDB to read in `format`: gs:// objects, with wildcards expanded, are downloaded into
// `downloads`, and a gzip file is moved or copied there under a name ending in .gz, since DuckDB
// selects gzip by suffix. Returns their paths. Throws ApiError::Invalid for a missing or
// unsupported URI.
std::vector<std::string> StageLoadSources(const std::vector<std::string>& uris, FileFormat format,
                                          GcsClient& gcs, TemporaryFiles& downloads);

// Stages the NEWLINE_DELIMITED_JSON files `paths` for LoadQuery with `schema`. DuckDB reads a
// JSON number as a DOUBLE, so the NUMERIC and BIGNUMERIC values of `schema` are rewritten into
// `downloads` as strings that LoadQuery casts exactly. Returns `paths` when `schema` has neither
// type. Throws ApiError::Invalid for malformed JSON or an invalid BIGNUMERIC.
std::vector<std::string> StageJsonNumerics(const std::vector<std::string>& paths,
                                           const std::vector<FieldSchema>& schema,
                                           TemporaryFiles& downloads);

// Parquet files staged for LoadQuery, and the columns they have.
struct ParquetSources {
  std::vector<std::string> paths;
  ParquetColumn columns;
};

// Stages PARQUET files for LoadQuery. Copies files into `downloads` with wide DECIMAL
// annotations and GeoParquet metadata removed, so DuckDB reads their raw bytes. Keeps decimal
// types and geometry edge semantics in `columns` for schema inference and conversion.
// Rejects incompatible file schemas or unsupported GeoParquet encodings and coordinate systems.
// The columns take the shape BigQuery loads with or without `list_inference`.
ParquetSources StageParquetSources(const std::vector<std::string>& paths, TemporaryFiles& downloads,
                                   bool list_inference);

// Restores GeoParquet GEOGRAPHY types and modes and wide DECIMAL types in DuckDB's inferred
// schema. Wide decimals require BIGNUMERIC in decimalTargetTypes (`targets`).
void DetectParquetSchema(std::vector<FieldSchema>& schema, const ParquetColumn& columns,
                         const std::vector<DecimalTargetType>& targets);

// The query that reads the files `paths` using `options`, as the columns of `schema`,
// or as DuckDB detects them when
// `schema` is empty. PARQUET files take the `parquet` columns StageParquetSources found. Throws
// ApiError::Invalid for a Parquet column the emulator cannot load into its field.
std::string LoadQuery(const LoadOptions& options, const std::vector<std::string>& paths,
                      const std::vector<FieldSchema>& schema, const ParquetColumn& parquet = {});

}  // namespace bigquery_emulator_duckdb
