#pragma once

#include "src/backend_initializer.h"

namespace bigquery_emulator_duckdb {

// Production connector registration, kept out of the native backend defaults.
BackendInitializer ServiceBackendInitializer(BackendInitializer initialize);
void InitializeServiceDatabase(duckdb::DuckDB& database);

}  // namespace bigquery_emulator_duckdb
