#pragma once

#include <cstddef>

#include "absl/status/statusor.h"

namespace googlesql {
class Type;
class Value;
}  // namespace googlesql

namespace bigquery_emulator_duckdb {

class ResultRows;

// Converts DuckDB result values to GoogleSQL for scripts and the reference test driver,
// independently of the REST and Storage Read encodings.
class GoogleSqlValueReader {
 public:
  // Preserves ResultRows' NULL array policy. Returns an error for a value `type` cannot hold,
  // and throws std::out_of_range past the last row or column.
  static absl::StatusOr<googlesql::Value> Read(const ResultRows& rows, size_t row, size_t column,
                                               const googlesql::Type* type);
};

}  // namespace bigquery_emulator_duckdb
