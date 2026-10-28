#pragma once

#include "duckdb.h"

namespace bigquery_emulator_duckdb::collation_duckdb {

// Registers bq_collate(value, specification), COLLATE, which checks the specification and returns
// the value, and bq_collation_key(value, specification), the BLOB sort key that the translator
// compares, sorts and groups in place of a STRING value with a collation. With `bigquery`, they
// take only the specifications BigQuery takes.
void RegisterCollationFunctions(duckdb_connection connection, bool bigquery);

}  // namespace bigquery_emulator_duckdb::collation_duckdb
