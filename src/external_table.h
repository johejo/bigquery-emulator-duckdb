#pragma once

#include <vector>

#include "src/field_schema.h"
#include "src/gcs.h"
#include "src/statement_resources.h"
#include "src/table_metadata.h"

namespace bigquery_emulator_duckdb {

// The schema BigQuery detects for the Parquet sources of `definition`: the columns of the
// alphabetically last file, each REQUIRED or NULLABLE as the file declares it. Downloads the
// sources. Throws ApiError::Unsupported for a column whose type or name is not emulated.
std::vector<FieldSchema> DetectGcsExternalSchema(const GcsExternalData& definition,
                                                 GcsClient client);

// Captures only the persisted definition and a shared configured client; each statement owns
// its downloads and resolves wildcards anew. Resolution itself performs no network requests.
// Each source column must have the type DetectGcsExternalSchema gives it, and a REQUIRED column
// a REQUIRED source column.
TableRead GcsExternalRead(GcsExternalData definition, std::vector<FieldSchema> schema,
                          GcsClient client);

}  // namespace bigquery_emulator_duckdb
