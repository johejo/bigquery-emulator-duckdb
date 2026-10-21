#pragma once

#include "duckdb.h"
#include "nlohmann/json.hpp"

namespace bigquery_emulator_duckdb {

// The cell, {"v": ...}, of `row` of a result vector in BigQuery's wire format. A NULL array is
// an empty one, as BigQuery returns it, unless `null_arrays` is set.
nlohmann::json ToCell(duckdb_vector vector, duckdb_logical_type type, idx_t row, bool null_arrays);

}  // namespace bigquery_emulator_duckdb
