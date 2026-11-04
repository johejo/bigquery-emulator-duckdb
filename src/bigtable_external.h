#pragma once

#include <vector>

#include "src/field_schema.h"
#include "src/statement_resources.h"
#include "src/table_metadata.h"

namespace bigquery_emulator_duckdb {

// Bigtable external table reads. Options follow BigtableOptions in
// third_party/bigquery_docs/reference/rest/v2/tables.md.
std::vector<FieldSchema> DetectBigtableSchema(const GcsExternalData& definition);
TableRead BigtableExternalRead(GcsExternalData definition, std::vector<FieldSchema> schema);

}  // namespace bigquery_emulator_duckdb
