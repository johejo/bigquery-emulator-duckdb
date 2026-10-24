#pragma once

#include "duckdb.h"

namespace bigquery_emulator_duckdb::aead_duckdb {

void RegisterAeadFunctions(duckdb_connection connection);

}  // namespace bigquery_emulator_duckdb::aead_duckdb
