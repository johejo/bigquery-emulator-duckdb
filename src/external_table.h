#pragma once

#include <vector>

#include "src/field_schema.h"
#include "src/gcs.h"
#include "src/statement_resources.h"
#include "src/table_metadata.h"

namespace bigquery_emulator_duckdb {

// Captures only the persisted definition and a shared configured client; each statement owns
// its downloads and resolves wildcards anew. Resolution itself performs no network requests.
TableRead GcsExternalRead(GcsExternalData definition, std::vector<FieldSchema> schema,
                          GcsClient client);

}  // namespace bigquery_emulator_duckdb
