#pragma once

#include "googlesql/public/value.h"

namespace googlesql {
class Type;
}

namespace bigquery_emulator_duckdb {

struct QueryResult;

// The one value of `type` that `result`, a query run with null_arrays set, returns, such as the
// value of a DDL option. Throws ApiError::UnsupportedQuery for a value the emulator cannot decode.
googlesql::Value ResultValue(const QueryResult& result, const googlesql::Type* type);

}  // namespace bigquery_emulator_duckdb
