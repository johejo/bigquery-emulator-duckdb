#pragma once

#include "duckdb.h"

struct ArrowArray;

namespace bigquery_emulator_duckdb {

struct FieldSchema;

// Appends a vector element in Storage Read wire layout, without query-cell materialization.
void AppendArrowVector(ArrowArray* array, const FieldSchema& field, duckdb_vector vector,
                       duckdb_logical_type type, idx_t row);

}  // namespace bigquery_emulator_duckdb
