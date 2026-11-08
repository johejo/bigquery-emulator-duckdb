#include "src/duckpgq.h"

#include <exception>

#include "duckdb/main/database.hpp"
#include "duckpgq_extension.hpp"
#include "src/backend_error.h"

namespace bigquery_emulator_duckdb {
void RegisterDuckPGQ(duckdb::DuckDB& database) {
  try {
    database.LoadStaticExtension<duckdb::DuckpgqExtension>();
  } catch (const std::exception& error) {
    throw BackendError(error.what());
  }
}
}  // namespace bigquery_emulator_duckdb
