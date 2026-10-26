#pragma once

#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "duckdb.h"
#include "googlesql/public/types/type_factory.h"
#include "googlesql/public/value.h"
#include "src/duckdb_handle.h"

namespace bigquery_emulator_duckdb::functions_duckdb {

// Arguments read as GoogleSQL values, preserving nested NULLs and original STRUCT names.
// Paired columns carry struct_pack(value := ..., names := <JSON array of field names>).
// An empty paired list means every column is paired. Types outlive the values returned by Get.
class AnyArguments {
 public:
  explicit AnyArguments(duckdb_data_chunk input, const std::vector<idx_t>& paired = {});
  [[nodiscard]] absl::StatusOr<googlesql::Value> Get(idx_t column, idx_t row) const;
  [[nodiscard]] const googlesql::Type* Type(idx_t column) const;

 private:
  std::vector<duckdb_vector> vectors_;
  googlesql::TypeFactory factory_;
  std::vector<LogicalType> duck_types_;
  std::vector<const googlesql::Type*> types_;
  absl::Status status_;
};

}  // namespace bigquery_emulator_duckdb::functions_duckdb
