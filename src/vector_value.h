#pragma once

// Reads the values of DuckDB result vectors, which the encodings of query results and Storage
// Read snapshots share.

#include <string>

#include "duckdb.h"
#include "src/duckdb_handle.h"

namespace bigquery_emulator_duckdb {

// Whether `row` of `vector` is NULL.
bool IsNull(duckdb_vector vector, idx_t row);

// The elements of `row` of a LIST or ARRAY vector, which must not be NULL.
class ListElements {
 public:
  ListElements(duckdb_vector vector, duckdb_logical_type type, idx_t row);

  [[nodiscard]] idx_t size() const { return entry_.length; }
  // The vector that holds the elements, and the row in it of element `i`.
  [[nodiscard]] duckdb_vector child() const { return child_; }
  [[nodiscard]] idx_t row(idx_t i) const { return entry_.offset + i; }
  [[nodiscard]] duckdb_logical_type type() const { return type_.get(); }

 private:
  duckdb_vector child_ = nullptr;
  LogicalType type_;
  duckdb_list_entry entry_{};
};

// Whether `row` of a RANGE's bound vector, of DuckDB type `type`, a DATE or a timestamp, is a
// finite value rather than an unbounded end.
bool IsFiniteBound(duckdb_vector bound, duckdb_logical_type type, idx_t row);

// The value of `row` of a vector, reconstructed through the C API so that DuckDB formats scalar
// values itself.
Value VectorValue(duckdb_vector vector, duckdb_logical_type type, idx_t row);

// DuckDB's text for `value`.
std::string ValueString(duckdb_value value);

}  // namespace bigquery_emulator_duckdb
