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

// Stages the PARQUET files `paths` for LoadQuery. DuckDB reads a DECIMAL wider than 38 digits as
// a DOUBLE, so a file with one is copied into `downloads` with the DECIMAL annotations of those
// columns removed from its footer, for DuckDB to read their bytes and LoadQuery to convert them
// exactly. Throws ApiError::Invalid for a file that is not Parquet, and for files whose schemas
// differ when one has such a column. The columns take the shape BigQuery loads with or without
// `list_inference`.
ParquetSources StageParquetDecimals(const std::vector<std::string>& paths,
                                    TemporaryFiles& downloads, bool list_inference);

// Gives the fields of `schema`, as DuckDB detects it from staged Parquet files with `columns`,
// that hold a DECIMAL wider than 38 digits the type that BigQuery detects for them under the
// decimalTargetTypes in `targets`. Throws ApiError::Invalid when
// that type is not BIGNUMERIC, which the emulator does not support.
void DetectParquetDecimals(std::vector<FieldSchema>& schema, const ParquetColumn& columns,
                           const std::vector<DecimalTargetType>& targets);

// The query that reads the files `paths` using `options`, as the columns of `schema`,
// or as DuckDB detects them when
// `schema` is empty. PARQUET files take the `parquet` columns StageParquetDecimals found. Throws
// ApiError::Invalid for a Parquet column the emulator cannot load into its field.
std::string LoadQuery(const LoadOptions& options, const std::vector<std::string>& paths,
                      const std::vector<FieldSchema>& schema, const ParquetColumn& parquet = {});

}  // namespace bigquery_emulator_duckdb
