#pragma once

#include <chrono>
#include <cstddef>

namespace bigquery_emulator_duckdb::backend_functions {

// What a JavaScript UDF may use: how long one call may run, and how much memory the calls of one
// query may allocate on each DuckDB thread. BigQuery documents neither exactly; its timeouts "can
// be as short as 5 minutes", and its memory per query is limited.
struct JavaScriptLimits {
  std::chrono::milliseconds call_time = std::chrono::minutes(5);
  size_t memory = size_t{256} << 20U;
};

}  // namespace bigquery_emulator_duckdb::backend_functions
