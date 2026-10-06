#pragma once

#include "absl/status/statusor.h"
#include "googlesql/public/value.h"
#include "nlohmann/json_fwd.hpp"

namespace googlesql {
class Type;
}

namespace bigquery_emulator_duckdb {

// Decodes the value of a result cell, its "v", as the emulator returns it to clients, into a
// GoogleSQL value of `type`. A null cell is a NULL of `type`, including an ARRAY.
absl::StatusOr<googlesql::Value> CellValue(const googlesql::Type* type, const nlohmann::json& cell);

}  // namespace bigquery_emulator_duckdb
