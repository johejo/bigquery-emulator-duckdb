#pragma once

#include <string>
#include <vector>

#include "nlohmann/json.hpp"
#include "src/field_schema.h"

namespace bigquery_emulator_duckdb {

// How an extract job writes CSV and newline-delimited JSON.
struct TextExtractOptions {
  bool json = false;  // NEWLINE_DELIMITED_JSON rather than CSV.
  bool gzip = false;
  std::string field_delimiter = ",";
  bool print_header = true;
};

// Writes `rows`, in the wire format of QueryResult with TIMESTAMP cells as epoch microseconds, to
// the file `path` in BigQuery's export format. Throws ApiError for a schema the format cannot
// hold or the emulator does not support.
void WriteTextExtract(const std::vector<FieldSchema>& schema,
                      const std::vector<nlohmann::json>& rows, const TextExtractOptions& options,
                      const std::string& path);

// The DuckDB select list that gives each column of a table with `schema` the Parquet type that
// BigQuery's Parquet export writes. Throws ApiError for a type the emulator does not export.
std::string ParquetExtractColumns(const std::vector<FieldSchema>& schema);

// Declares the BIGNUMERIC columns of `schema` in the Parquet file `path`, which DuckDB wrote from
// ParquetExtractColumns, as DECIMAL(76, 38), the type BigQuery's Parquet export writes.
void AnnotateParquetBigNumerics(const std::string& path, const std::vector<FieldSchema>& schema);

}  // namespace bigquery_emulator_duckdb
