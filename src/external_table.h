#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "src/field_schema.h"
#include "src/gcs.h"
#include "src/statement_resources.h"
#include "src/table_metadata.h"

namespace bigquery_emulator_duckdb {

// The schema BigQuery detects for the sources of `definition`, the external table `table_id`,
// from the alphabetically last file. For Parquet, it has the file's columns, with LIST inference
// as configured, a group as a RECORD, a repeated field as REPEATED, and other fields REQUIRED or
// NULLABLE as the file declares them; CSV and JSON detect it as DetectCsv and DetectJson do.
// Downloads the sources. Throws ApiError::Unsupported for a column whose type or name is not
// emulated.
std::vector<FieldSchema> DetectGcsExternalSchema(const GcsExternalData& definition,
                                                 std::string_view table_id, GcsClient client);

// Captures only the persisted definition and a shared configured client; each statement owns
// its downloads and resolves wildcards anew. Resolution itself performs no network requests.
// Each source column must have the type DetectGcsExternalSchema gives it and be REPEATED where
// the schema is; a source can lack NULLABLE columns and fields, and have fields the schema does
// not. A NULL value of a REQUIRED column or field reads as NULL. JSON read errors name the table
// `table_name`, as BigQuery's do.
// The partition keys of the Hive-partitioned external table `table_id` defined as `definition`,
// with `declared` those of WITH PARTITION COLUMNS, from the paths its sources list. Throws
// ApiError as DetectHiveKeys does.
std::vector<FieldSchema> DetectGcsPartitionKeys(const GcsExternalData& definition,
                                                const std::vector<FieldSchema>& declared,
                                                std::string_view table_id, GcsClient client);

TableRead GcsExternalRead(GcsExternalData definition, std::vector<FieldSchema> schema,
                          std::string table_name, GcsClient client);

}  // namespace bigquery_emulator_duckdb
