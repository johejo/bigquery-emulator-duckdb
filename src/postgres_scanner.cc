#include "src/postgres_scanner.h"

#include <exception>

#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"
#include "postgres_scanner_extension.hpp"
#include "src/backend_error.h"

namespace bigquery_emulator_duckdb {

void RegisterPostgresScanner(duckdb::DuckDB& database) {
  try {
    database.LoadStaticExtension<PostgresScannerExtension>();
    // A NUMERIC without a precision, or with one beyond DuckDB's DECIMAL, is read as text rather
    // than as a DOUBLE, which would lose digits before EXTERNAL_QUERY converts it.
    const auto result =
        duckdb::Connection(database).Query("SET GLOBAL pg_numeric_as_varchar = true");
    if (result->HasError()) {
      throw BackendError(result->GetError());
    }
  } catch (const std::exception& error) {
    throw BackendError(error.what());
  }
}

}  // namespace bigquery_emulator_duckdb
