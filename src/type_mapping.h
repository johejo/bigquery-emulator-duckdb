#pragma once

// How a column type is spelled in each of the emulator's three vocabularies: BigQuery's
// TableFieldSchema, GoogleSQL's types, and DuckDB's.

#include <optional>
#include <string>

#include "absl/status/statusor.h"
#include "src/field_schema.h"

namespace googlesql {
class Type;
class TypeFactory;
class TypeParameters;
}  // namespace googlesql

namespace bigquery_emulator_duckdb {

// Maps a BigQuery TableFieldSchema to the GoogleSQL type of the column.
absl::StatusOr<const googlesql::Type*> GoogleSqlType(const FieldSchema& field,
                                                     googlesql::TypeFactory* type_factory);

// The inverse of GoogleSqlType: describes a column of GoogleSQL type `type` the way BigQuery
// reports it in a TableSchema. Struct fields without a name get BigQuery's `_field_n`. Fails
// for a type BigQuery has no TableFieldSchema for, such as an array of arrays.
absl::StatusOr<FieldSchema> BigQueryFieldSchema(const std::string& name,
                                                const googlesql::Type* type);

// The DuckDB type of `type`, narrowed by `parameters` when a column definition gives some, or
// nullopt for a type the translator does not support. DuckDB ignores lengths, so STRING(L) and
// BYTES(L) lose them; NUMERIC(P, S) keeps its rounding as DECIMAL(P, S). DuckDB structs need
// distinct field names, which anonymous BigQuery fields lack.
std::optional<std::string> DuckDbType(const googlesql::Type* type,
                                      const googlesql::TypeParameters* parameters = nullptr);

// The DuckDB type of a column described by a BigQuery TableFieldSchema, as DuckDbType maps its
// GoogleSQL type, except that GEOGRAPHY is stored as its text.
absl::StatusOr<std::string> DuckDbColumnType(const FieldSchema& field);

}  // namespace bigquery_emulator_duckdb
