#pragma once

// DuckDB C API helpers shared by the backend's sources.

#include <memory>
#include <string>
#include <utility>

#include "duckdb.h"

namespace bigquery_emulator_duckdb {

// C API handles must be released even when materialization or a query throws.
template <typename T, void (*Destroy)(T*)>
class Handle {
 public:
  explicit Handle(T handle = nullptr) : handle_(handle) {}
  ~Handle() { Destroy(&handle_); }
  Handle(Handle&& other) noexcept : handle_(other.release()) {}
  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;
  T get() const { return handle_; }
  T* out() { return &handle_; }
  T release() { return std::exchange(handle_, nullptr); }

 private:
  T handle_;
};

using LogicalType = Handle<duckdb_logical_type, duckdb_destroy_logical_type>;
using Value = Handle<duckdb_value, duckdb_destroy_value>;
using Connection = Handle<duckdb_connection, duckdb_disconnect>;
using Prepared = Handle<duckdb_prepared_statement, duckdb_destroy_prepare>;
using Chunk = Handle<duckdb_data_chunk, duckdb_destroy_data_chunk>;
struct DuckFree {
  void operator()(const void* pointer) const { duckdb_free(const_cast<void*>(pointer)); }
};
using DuckString = std::unique_ptr<const char, DuckFree>;

template <typename T>
inline T VectorElement(duckdb_vector vector, idx_t row) {
  return static_cast<const T*>(duckdb_vector_get_data(vector))[row];
}

inline std::string VectorString(duckdb_vector vector, idx_t row) {
  auto value = VectorElement<duckdb_string_t>(vector, row);
  return {duckdb_string_t_data(&value), duckdb_string_t_length(value)};
}

}  // namespace bigquery_emulator_duckdb
