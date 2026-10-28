#pragma once

#include "duckdb.h"

namespace bigquery_emulator_duckdb::differential_privacy_duckdb {

void RegisterDifferentialPrivacyFunctions(duckdb_connection connection);

}  // namespace bigquery_emulator_duckdb::differential_privacy_duckdb
