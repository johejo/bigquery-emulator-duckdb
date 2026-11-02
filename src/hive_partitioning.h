#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "src/field_schema.h"
#include "src/table_metadata.h"

namespace bigquery_emulator_duckdb {

// The partition keys of a table partitioned as `partitioning` whose sources are `uris`, as
// BigQuery detects them from the Hive layout of their paths under the source URI prefix: AUTO
// detects INTEGER, DATE and TIMESTAMP keys and STRING ones otherwise, STRINGS only STRING ones,
// and CUSTOM takes the keys its prefix or `declared` gives and checks the values. Throws
// ApiError with BigQuery's messages for a layout it rejects, naming the table `table_id`.
std::vector<FieldSchema> DetectHiveKeys(const HivePartitioning& partitioning,
                                        const std::vector<FieldSchema>& declared,
                                        const std::vector<std::string>& uris,
                                        std::string_view table_id);

// The values of `keys` for the source `uri`, in a form a CAST to each key's type reads, or none
// for Hive's default partition, which holds NULL. Throws ApiError as DetectHiveKeys does.
std::vector<std::optional<std::string>> HiveValues(const HivePartitioning& partitioning,
                                                   const std::vector<FieldSchema>& keys,
                                                   const std::string& uri,
                                                   std::string_view table_id);

// The keys a CUSTOM source URI prefix such as gs://bucket/table/{dt:DATE}/{n:INTEGER} encodes,
// or none for a prefix that encodes none. Throws ApiError::Invalid for a type BigQuery rejects.
std::vector<FieldSchema> CustomPrefixKeys(const std::string& prefix);

// Throws ApiError::Invalid for a partition key type BigQuery rejects, named `name`.
void CheckHiveKeyType(FieldType type, std::string_view name);

}  // namespace bigquery_emulator_duckdb
