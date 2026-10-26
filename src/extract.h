#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include "absl/status/statusor.h"
#include "src/extract_options.h"
#include "src/field_schema.h"

namespace googlesql {
class Type;
class Value;
}  // namespace googlesql

namespace bigquery_emulator_duckdb {

// The value of column `column` of row `row` of an extracted table as a GoogleSQL value of `type`.
using ExtractValue = std::function<absl::StatusOr<googlesql::Value>(size_t row, size_t column,
                                                                    const googlesql::Type* type)>;

// Writes `row_count` rows, whose values `value` gives, to the file `path` in BigQuery's export
// format. Throws ApiError for a schema the format cannot hold or the emulator does not support.
void WriteTextExtract(const std::vector<FieldSchema>& schema, size_t row_count,
                      const ExtractValue& value, const TextExtractFormatOptions& options,
                      ExtractCompression compression, const std::string& path);

// The DuckDB select list that gives each column of a table with `schema` the Parquet type that
// BigQuery's Parquet export writes. Throws ApiError for a type the emulator does not export.
std::string ParquetExtractColumns(const std::vector<FieldSchema>& schema);

// Declares the BIGNUMERIC columns of `schema` in the Parquet file `path`, which DuckDB wrote from
// ParquetExtractColumns, as DECIMAL(76, 38), the type BigQuery's Parquet export writes.
void AnnotateParquetBigNumerics(const std::string& path, const std::vector<FieldSchema>& schema);

}  // namespace bigquery_emulator_duckdb
