#include "src/service_backend.h"

#include <utility>

#include "duckdb.h"
#include "src/backend_initializer.h"
#include "src/mysql_query.h"
#include "src/postgres_scanner.h"
#include "src/spanner_query.h"

namespace bigquery_emulator_duckdb {

BackendInitializer ServiceBackendInitializer(BackendInitializer initialize) {
  return [initialize = std::move(initialize)](duckdb_connection connection) {
    RegisterSpannerQuery(connection);
    if (initialize) {
      initialize(connection);
    }
  };
}

void InitializeServiceDatabase(duckdb_database database) {
  RegisterPostgresScanner(database);
  RegisterMySqlScanner(database);
}

}  // namespace bigquery_emulator_duckdb
