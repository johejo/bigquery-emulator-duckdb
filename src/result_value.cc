#include "src/result_value.h"

#include <utility>

#include "absl/status/statusor.h"
#include "googlesql/public/types/type.h"
#include "googlesql/public/value.h"
#include "src/api_error.h"
#include "src/backend.h"

namespace bigquery_emulator_duckdb {

googlesql::Value ResultValue(const QueryResult& result, const googlesql::Type* type) {
  absl::StatusOr<googlesql::Value> value = result.rows.GoogleSqlValue(0, 0, type);
  if (!value.ok()) {
    throw ApiError::UnsupportedQuery("option values of type " + type->DebugString());
  }
  return *std::move(value);
}

}  // namespace bigquery_emulator_duckdb
