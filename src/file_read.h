#pragma once

#include <string>
#include <vector>

#include "src/field_schema.h"
#include "src/file_read_options.h"
#include "src/gcs.h"
#include "src/parquet_metadata.h"
#include "src/temporary_files.h"

namespace bigquery_emulator_duckdb {

// One concrete source, after wildcard expansion. Format staging replaces only path, keeping
// uri available for source attribution, including _FILE_NAME. The caller owns
// the TemporaryFiles that keep path alive; uri is never a generated temporary path.
struct FileSource {
  std::string uri;
  std::string path;
};

// Downloads expanded gs:// objects and resolves local paths. Detects gzip by its bytes and
// stages it with a .gz suffix for DuckDB. Throws ApiError::Invalid for missing or unsupported
// URIs. Callers must enforce which URI schemes their operation accepts.
std::vector<FileSource> StageFileSources(const std::vector<std::string>& uris, FileFormat format,
                                         GcsClient& gcs, TemporaryFiles& downloads);

// The contents of the file at `path`, decompressed when it is gzip. Throws ApiError::Invalid when
// it cannot be read.
std::string ReadMaybeGzip(const std::string& path);

// Rewrites JSON NUMERIC and BIGNUMERIC numbers as strings for exact conversion by FileReadQuery.
// Returns the sources unchanged when the schema contains neither type. Malformed JSON and
// invalid BIGNUMERIC values throw ApiError::Invalid.
std::vector<FileSource> StageJsonNumerics(const std::vector<FileSource>& sources,
                                          const std::vector<FieldSchema>& schema,
                                          TemporaryFiles& downloads);

struct ParquetSources {
  std::vector<FileSource> sources;
  ParquetColumn columns;
};

// Removes wide DECIMAL annotations and GeoParquet metadata in staged copies, preserving
// conversion metadata in columns. Rejects incompatible schemas and unsupported GeoParquet
// encodings or coordinate systems. list_inference selects the schema shape.
ParquetSources StageParquetSources(const std::vector<FileSource>& inputs, TemporaryFiles& downloads,
                                   bool list_inference);

// Restores GEOGRAPHY and wide DECIMAL types and REQUIRED modes in an inferred schema. Wide
// decimals require BIGNUMERIC in targets.
void DetectParquetSchema(std::vector<FieldSchema>& schema, const ParquetColumn& columns,
                         const std::vector<DecimalTargetType>& targets);

// Reads staged sources with schema conversion, or infers columns when schema is empty.
// Does not prepare or execute SQL, acquire files, or apply destination-table policies.
std::string FileReadQuery(const FileReadOptions& options, const std::vector<FileSource>& sources,
                          const std::vector<FieldSchema>& schema,
                          const ParquetColumn& parquet = {});

}  // namespace bigquery_emulator_duckdb
