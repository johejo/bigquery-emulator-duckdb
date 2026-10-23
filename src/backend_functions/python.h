#pragma once

#include <chrono>
#include <cstddef>
#include <string>

#include "duckdb.h"

namespace bigquery_emulator_duckdb::backend_functions {

struct PythonOptions {
  // Empty finds the bundled worker in Bazel runfiles or next to the executable.
  std::string worker;
  std::chrono::milliseconds call_time = std::chrono::minutes(5);
  size_t memory = size_t{256} << 20U;
};

void RegisterPythonFunctions(duckdb_connection connection, const PythonOptions& options = {});

}  // namespace bigquery_emulator_duckdb::backend_functions
