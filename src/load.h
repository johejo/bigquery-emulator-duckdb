#pragma once

#include <string>
#include <vector>

#include "nlohmann/json_fwd.hpp"
#include "src/field_schema.h"
#include "src/gcs.h"
#include "src/temporary_files.h"

namespace bigquery_emulator_duckdb {

// Stages the files that the sourceUris of `config`, a load job's configuration.load, name for
// DuckDB to read in `format`: gs:// objects, with wildcards expanded, are downloaded into
// `downloads`, and a gzip file is moved or copied there under a name ending in .gz, since DuckDB
// selects gzip by suffix. Returns their paths. Throws ApiError::Invalid for a missing or
// unsupported URI.
std::vector<std::string> StageLoadSources(const nlohmann::json& config, const std::string& format,
                                          GcsClient& gcs, TemporaryFiles& downloads);

// Stages the NEWLINE_DELIMITED_JSON files `paths` for LoadQuery with `schema`. DuckDB reads a
// JSON number as a DOUBLE, so the NUMERIC and BIGNUMERIC values of `schema` are rewritten into
// `downloads` as strings that LoadQuery casts exactly. Returns `paths` when `schema` has neither
// type. Throws ApiError::Invalid for malformed JSON or an invalid BIGNUMERIC.
std::vector<std::string> StageJsonNumerics(const std::vector<std::string>& paths,
                                           const std::vector<FieldSchema>& schema,
                                           TemporaryFiles& downloads);

// The query that reads the files `paths` in `format`, CSV, NEWLINE_DELIMITED_JSON or PARQUET,
// with the options of `config`, as the columns of `schema`, or as DuckDB detects them when
// `schema` is empty.
std::string LoadQuery(const std::string& format, const std::vector<std::string>& paths,
                      const nlohmann::json& config, const std::vector<FieldSchema>& schema);

}  // namespace bigquery_emulator_duckdb
