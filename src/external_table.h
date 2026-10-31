#pragma once

#include <vector>

#include "src/field_schema.h"
#include "src/gcs.h"
#include "src/statement_resources.h"
#include "src/table_metadata.h"

namespace bigquery_emulator_duckdb {

// The schema BigQuery detects for the Parquet sources of `definition`: the columns of the
// alphabetically last file, with LIST inference as configured, a group as a RECORD, a repeated
// field as REPEATED, and other fields REQUIRED or NULLABLE as the file declares them. Downloads
// the sources. Throws ApiError::Unsupported for a column whose type or name is not emulated.
std::vector<FieldSchema> DetectGcsExternalSchema(const GcsExternalData& definition,
                                                 GcsClient client);

// Captures only the persisted definition and a shared configured client; each statement owns
// its downloads and resolves wildcards anew. Resolution itself performs no network requests.
// Each source column must have the type DetectGcsExternalSchema gives it and be REPEATED where
// the schema is; a source can lack NULLABLE columns and fields, and have fields the schema does
// not. A NULL value of a REQUIRED column or field reads as NULL.
TableRead GcsExternalRead(GcsExternalData definition, std::vector<FieldSchema> schema,
                          GcsClient client);

}  // namespace bigquery_emulator_duckdb
