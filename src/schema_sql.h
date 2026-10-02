#pragma once

#include <optional>
#include <string>
#include <vector>

#include "nlohmann/json_fwd.hpp"
#include "src/field_schema.h"
#include "src/references.h"

namespace bigquery_emulator_duckdb {

// The DuckDB SQL that keeps BigQuery tables in DuckDB. Each function throws ApiError::Invalid
// for a schema or value BigQuery rejects or the emulator does not support.

// The DuckDB column type of a BigQuery TableFieldSchema.
std::string ToDuckDbType(const FieldSchema& field);

// A DuckDB column definition for `field`, which enforces REQUIRED as BigQuery does.
std::string ColumnDefinition(const FieldSchema& field);

// Column definitions for a table that holds `schema`, or nullopt when a type has no column
// type of its own in the emulator.
std::optional<std::string> ColumnDefinitions(const std::vector<FieldSchema>& schema);

// The DuckDB expression for `value`, a value of `field` in a tabledata.insertAll row. An unknown
// field of a RECORD is an error unless `ignore_unknown_values` is set.
std::string InsertValue(const nlohmann::json& value, const FieldSchema& field,
                        bool ignore_unknown_values);

// Checks that BigQuery lets the schema `current` of `table` become `updated` through tables.patch
// or tables.update, and returns the ALTER TABLE statements that make the change. Existing fields
// keep their order, name, type, type parameters and default; REQUIRED may become NULLABLE;
// NULLABLE and REPEATED fields may be added after them.
std::vector<std::string> SchemaUpdateStatements(const TableReference& table,
                                                const std::vector<FieldSchema>& current,
                                                const std::vector<FieldSchema>& updated);

}  // namespace bigquery_emulator_duckdb
