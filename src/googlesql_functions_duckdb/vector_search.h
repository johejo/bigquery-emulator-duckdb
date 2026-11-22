#pragma once

#include "duckdb.h"

namespace bigquery_emulator_duckdb::functions_duckdb {

// Registers bq_vector_search_options, which checks VECTOR_SEARCH options given as a query
// parameter, whose value the catalog does not have.
void RegisterVectorSearch(duckdb_connection connection);

}  // namespace bigquery_emulator_duckdb::functions_duckdb
