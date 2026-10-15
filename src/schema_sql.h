#pragma once

#include <functional>
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

// The DuckDB value of the RANGE `field` that `text`, a VARCHAR in the form "[start, end)", holds,
// where NULL and UNBOUNDED are unbounded ends and each other bound is cast to the element type.
std::string RangeFromText(const std::string& text, const FieldSchema& field);

// The DuckDB value `sql` of the REPEATED `field` with each element replaced by
// `element(name, element_field, depth + 1)`, where `name` is the lambda's parameter, _e and
// `depth`, and `element_field` is `field` as NULLABLE.
std::string MapElements(
    const std::string& sql, const FieldSchema& field, int depth,
    const std::function<std::string(const std::string&, const FieldSchema&, int)>& element);

// The DuckDB value `sql` of the RECORD `field` with each field `child` replaced by
// `value(child_sql, child)`. A NULL record stays NULL.
std::string MapFields(
    const std::string& sql, const FieldSchema& field,
    const std::function<std::string(const std::string&, const FieldSchema&)>& value);

// Checks that BigQuery lets the schema `current` of `table` become `updated` through tables.patch
// or tables.update, and returns the ALTER TABLE statements that make the change. Existing fields
// keep their order, name, type, type parameters and default; REQUIRED may become NULLABLE;
// NULLABLE and REPEATED fields may be added after them.
std::vector<std::string> SchemaUpdateStatements(const TableReference& table,
                                                const std::vector<FieldSchema>& current,
                                                const std::vector<FieldSchema>& updated);

}  // namespace bigquery_emulator_duckdb
