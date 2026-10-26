#pragma once

namespace bigquery_emulator_duckdb {

// Runs the private Python protocol on fd 3. argv contains the executable and memory limit.
int PythonWorkerMain(int argc, char** argv);

}  // namespace bigquery_emulator_duckdb
