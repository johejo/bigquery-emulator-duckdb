#pragma once

#include <chrono>
#include <cstddef>
#include <string>

namespace bigquery_emulator_duckdb::udf {

struct PythonOptions {
  // Empty re-executes the emulator in its internal worker mode. External workers receive
  // the memory limit in bytes as their only argument and use the protocol socket on fd 3.
  std::string worker;
  std::chrono::milliseconds call_time = std::chrono::minutes(5);
  size_t memory = size_t{256} << 20U;
};

}  // namespace bigquery_emulator_duckdb::udf
