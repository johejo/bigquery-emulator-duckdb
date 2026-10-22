#include "src/duckpgq.h"

#include <exception>

#include "duckdb.h"
#include "duckdb/main/capi/capi_internal.hpp"
#include "duckdb/main/database.hpp"
#include "duckpgq_extension.hpp"
#include "src/backend_error.h"

namespace bigquery_emulator_duckdb {
void RegisterDuckPGQ(duckdb_database database) {
  try {
    reinterpret_cast<duckdb::DatabaseWrapper*>(database)
        ->database->LoadStaticExtension<duckdb::DuckpgqExtension>();
  } catch (const std::exception& error) {
    throw BackendError(error.what());
  }
}
}  // namespace bigquery_emulator_duckdb
