#pragma once

#include <string>
#include <vector>

#include "src/field_schema.h"
#include "src/statement_resources.h"
#include "src/table_metadata.h"

namespace bigquery_emulator_duckdb {

// Bigtable external table reads. Options follow BigtableOptions in
// third_party/bigquery_docs/reference/rest/v2/tables.md.
std::vector<FieldSchema> DetectBigtableSchema(const GcsExternalData& definition);
// `table_name` is the external table's, which read errors name.
TableRead BigtableExternalRead(GcsExternalData definition, std::vector<FieldSchema> schema,
                               std::string table_name);

}  // namespace bigquery_emulator_duckdb
