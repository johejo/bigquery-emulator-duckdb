#pragma once

#include "duckdb.h"
#include "src/backend_initializer.h"

namespace bigquery_emulator_duckdb {

// Production connector registration, kept out of the native backend defaults.
BackendInitializer ServiceBackendInitializer(BackendInitializer initialize);
void InitializeServiceDatabase(duckdb_database database);

}  // namespace bigquery_emulator_duckdb
