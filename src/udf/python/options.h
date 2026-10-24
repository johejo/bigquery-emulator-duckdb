#pragma once

#include <chrono>
#include <cstddef>
#include <string>

namespace bigquery_emulator_duckdb::udf {

struct PythonOptions {
  // Empty finds the bundled worker in Bazel runfiles or next to the executable.
  std::string worker;
  std::chrono::milliseconds call_time = std::chrono::minutes(5);
  size_t memory = size_t{256} << 20U;
};

}  // namespace bigquery_emulator_duckdb::udf
