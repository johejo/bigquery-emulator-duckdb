#pragma once

// Read-only access to result vectors. ResultChunks normalizes their representation before
// publishing them, so readers never flatten shared buffers.

#include <cstddef>
#include <string>
#include <string_view>

#include "duckdb/common/types.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/vector/flat_vector.hpp"

namespace bigquery_emulator_duckdb {

bool IsNull(const duckdb::Vector& vector, size_t row);

template <typename T>
T VectorElement(const duckdb::Vector& vector, size_t row) {
  return duckdb::FlatVector::GetData<T>(vector)[row];
}

std::string_view VectorStringView(const duckdb::Vector& vector, size_t row);
std::string VectorString(const duckdb::Vector& vector, size_t row);

bool IsListLike(const duckdb::LogicalType& type);
const duckdb::LogicalType& ElementType(const duckdb::LogicalType& type);

// The elements of a non-NULL LIST or ARRAY row. Borrows the normalized result vector.
class ListElements {
 public:
  ListElements(const duckdb::Vector& vector, const duckdb::LogicalType& type, size_t row);
  [[nodiscard]] size_t size() const { return entry_.length; }
  [[nodiscard]] const duckdb::Vector& child() const { return child_; }
  [[nodiscard]] size_t row(size_t i) const { return entry_.offset + i; }
  [[nodiscard]] const duckdb::LogicalType& type() const { return type_; }

 private:
  const duckdb::Vector& child_;
  const duckdb::LogicalType& type_;
  duckdb::list_entry_t entry_;
};

// Whether a DATE or timestamp RANGE bound is finite, rather than unbounded.
bool IsFiniteBound(const duckdb::Vector& bound, const duckdb::LogicalType& type, size_t row);

// DuckDB retains responsibility for restoring and formatting values of arbitrary types.
duckdb::Value VectorValue(const duckdb::Vector& vector, const duckdb::LogicalType& type,
                          size_t row);
std::string ValueString(const duckdb::Value& value);

}  // namespace bigquery_emulator_duckdb
