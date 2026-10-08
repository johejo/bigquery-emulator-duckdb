#pragma once

// How a column type is spelled in each of the emulator's three vocabularies: BigQuery's
// TableFieldSchema, GoogleSQL's types, and DuckDB's.

#include <optional>
#include <string>
#include <vector>

#include "absl/status/statusor.h"
#include "src/field_schema.h"

namespace googlesql {
class Type;
class StructType;
class TypeFactory;
class TypeParameters;
}  // namespace googlesql

namespace bigquery_emulator_duckdb {

// Whether a type contains INTERVAL, including array elements and struct fields.
bool HasInterval(const googlesql::Type* type);

// Maps a BigQuery TableFieldSchema to the GoogleSQL type of the column.
absl::StatusOr<const googlesql::Type*> GoogleSqlType(const FieldSchema& field,
                                                     googlesql::TypeFactory* type_factory);

// The inverse of GoogleSqlType: describes a column of GoogleSQL type `type` the way BigQuery
// reports it in a TableSchema. Struct fields without a name get BigQuery's `_field_n`. Fails
// for a type BigQuery has no TableFieldSchema for, such as an array of arrays.
absl::StatusOr<FieldSchema> BigQueryFieldSchema(const std::string& name,
                                                const googlesql::Type* type);

// Field names for a nonempty DuckDB struct. Preserve names when they are distinct and nonempty;
// otherwise use positional internal names for every field. GoogleSQL retains the original names.
std::vector<std::string> DuckDbStructFieldNames(const googlesql::StructType* type);

// The DuckDB type of `type`, narrowed by `parameters` when a column definition gives some, or
// nullopt for a type the translator does not support. DuckDB ignores lengths, so STRING(L) and
// BYTES(L) lose them; NUMERIC(P, S) keeps its rounding as DECIMAL(P, S), and BIGNUMERIC(P, S) as
// the type BigNumericTypeName names. Structs use DuckDbStructFieldNames; empty structs are
// unsupported. INTERVAL is available only for query values.
std::optional<std::string> DuckDbType(const googlesql::Type* type,
                                      const googlesql::TypeParameters* parameters = nullptr);

// The DuckDB type of a column described by a BigQuery TableFieldSchema, as DuckDbType maps its
// GoogleSQL type and the precision and scale the schema gives, except that GEOGRAPHY is stored as
// its text. Stored structs still require distinct nonempty field names.
absl::StatusOr<std::string> DuckDbColumnType(const FieldSchema& field);

}  // namespace bigquery_emulator_duckdb
