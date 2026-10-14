#pragma once

#include "absl/status/statusor.h"
#include "googlesql/public/value.h"
#include "nlohmann/json_fwd.hpp"

namespace googlesql {
class Type;
}

namespace bigquery_emulator_duckdb {

struct QueryResult;

// Decodes the value of a result cell, its "v", as the emulator returns it to clients, into a
// GoogleSQL value of `type`. A null cell is a NULL of `type`, including an ARRAY.
absl::StatusOr<googlesql::Value> CellValue(const googlesql::Type* type, const nlohmann::json& cell);

// The one value of `type` that `result`, a query run with null_arrays set, returns, such as the
// value of a DDL option. Throws ApiError::UnsupportedQuery for a value the emulator cannot decode.
googlesql::Value ResultValue(const QueryResult& result, const googlesql::Type* type);

}  // namespace bigquery_emulator_duckdb
