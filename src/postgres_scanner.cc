#include "src/postgres_scanner.h"

#include <exception>

#include "duckdb.h"
#include "duckdb/main/capi/capi_internal.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"
#include "postgres_scanner_extension.hpp"
#include "src/backend_error.h"

namespace bigquery_emulator_duckdb {

void RegisterPostgresScanner(duckdb_database database) {
  // The C API offers no way to load a statically linked extension, so this reaches the C++
  // instance behind the handle, which the C API keeps in a DatabaseWrapper.
  try {
    duckdb::DuckDB& instance = *reinterpret_cast<duckdb::DatabaseWrapper*>(database)->database;
    instance.LoadStaticExtension<PostgresScannerExtension>();
    // A NUMERIC without a precision, or with one beyond DuckDB's DECIMAL, is read as text rather
    // than as a DOUBLE, which would lose digits before EXTERNAL_QUERY converts it.
    const auto result =
        duckdb::Connection(instance).Query("SET GLOBAL pg_numeric_as_varchar = true");
    if (result->HasError()) {
      throw BackendError(result->GetError());
    }
  } catch (const std::exception& error) {
    throw BackendError(error.what());
  }
}

}  // namespace bigquery_emulator_duckdb
