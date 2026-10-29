#pragma once

#include "duckdb.h"
#include "src/googlesql_collation/options.h"

namespace bigquery_emulator_duckdb::collation_duckdb {

// Registers bq_collation_key(value, specification), the BLOB sort key that the translator
// compares, sorts and groups in place of a STRING value with a collation, and
// bq_collation_distinct(values, specification, direction, nulls_first), the DISTINCT values of a
// list, which ARRAY_AGG and STRING_AGG(DISTINCT ...) return.
void RegisterCollationFunctions(duckdb_connection connection,
                                const googlesql_collation::Options& options);

}  // namespace bigquery_emulator_duckdb::collation_duckdb
