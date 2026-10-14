#pragma once

// How a column type is spelled in each of the emulator's three vocabularies: BigQuery's
// TableFieldSchema, GoogleSQL's types, and DuckDB's.

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "absl/status/statusor.h"
#include "duckdb.h"
#include "src/field_schema.h"

namespace googlesql {
class Type;
class StructType;
class TypeFactory;
class TypeParameters;
}  // namespace googlesql

namespace bigquery_emulator_duckdb {

// The DuckDB STRUCT field names of a RANGE, whose unbounded ends are -infinity and infinity, so
// that DuckDB orders and compares RANGEs as BigQuery does. No struct field of a BigQuery column
// can have these names, which tell a RANGE from a STRUCT in results.
inline constexpr std::string_view kRangeStart = "$start";
inline constexpr std::string_view kRangeEnd = "$end";

// The DuckDB STRUCT field name of a STRUCT<>, which DuckDB cannot hold without a field. The field
// is TRUE in every non-NULL value, so DuckDB compares and groups them as BigQuery does. No struct
// field of a BigQuery column can have this name.
inline constexpr std::string_view kEmptyStruct = "$empty";

// GoogleSQL's error for a RANGE whose start does not precede its end.
inline constexpr std::string_view kRangeOrderError =
    "Range start element must be smaller than range end element";

// The DuckDB RANGE with the bounds `start` and `end`, DuckDB expressions of its element type, NULL
// for an unbounded end. Where `start` does not precede `end` it is `raise`, by default the SQL
// that raises kRangeOrderError. Each bound is evaluated more than once.
std::string DuckDbRange(std::string_view start, std::string_view end, std::string_view raise = "");

// Whether the DuckDB `type` is the STRUCT that keeps a RANGE.
bool IsRange(duckdb_logical_type type);

// Whether the DuckDB `type` is the STRUCT that keeps a STRUCT<>.
bool IsEmptyStruct(duckdb_logical_type type);

// Whether a type contains STRUCT<>, including array elements and struct fields.
bool HasEmptyStruct(const googlesql::Type* type);

// Whether a type contains RANGE, including array elements and struct fields.
bool HasRange(const googlesql::Type* type);

// Whether a type contains INTERVAL, including array elements and struct fields.
bool HasInterval(const googlesql::Type* type);

// Whether a type contains GEOGRAPHY, including array elements and struct fields.
bool HasGeography(const googlesql::Type* type);

// Maps a BigQuery TableFieldSchema to the GoogleSQL type of the column.
absl::StatusOr<const googlesql::Type*> GoogleSqlType(const FieldSchema& field,
                                                     googlesql::TypeFactory* type_factory);

// The inverse of GoogleSqlType: describes a column of GoogleSQL type `type` the way BigQuery
// reports it in a TableSchema. Struct fields without a name get BigQuery's `_field_n`. Fails
// for a type BigQuery has no TableFieldSchema for, such as an array of arrays or STRUCT<>.
absl::StatusOr<FieldSchema> BigQueryFieldSchema(const std::string& name,
                                                const googlesql::Type* type);

// Field names for a nonempty DuckDB struct. Preserve names when they are distinct and nonempty;
// otherwise use positional internal names for every field. GoogleSQL retains the original names.
std::vector<std::string> DuckDbStructFieldNames(const googlesql::StructType* type);

// The DuckDB type of `type`, narrowed by `parameters` when a column definition gives some, or
// nullopt for a type the translator does not support. DuckDB ignores lengths, so STRING(L) and
// BYTES(L) lose them; NUMERIC(P, S) keeps its rounding as DECIMAL(P, S), and BIGNUMERIC(P, S) as
// the type BigNumericTypeName names. Structs use DuckDbStructFieldNames; STRUCT<> is a STRUCT
// with the field kEmptyStruct, available only for query values, as is INTERVAL. A RANGE is a STRUCT
// with the fields kRangeStart and kRangeEnd.
std::optional<std::string> DuckDbType(const googlesql::Type* type,
                                      const googlesql::TypeParameters* parameters = nullptr);

// The numeric type parameters in a schema, including nested fields. Include STRING/BYTES
// lengths for default validation; storage omits them because DuckDB does not enforce them.
absl::StatusOr<googlesql::TypeParameters> FieldTypeParameters(const FieldSchema& field,
                                                              bool include_lengths = false);

// The DuckDB type of a column described by a BigQuery TableFieldSchema, as DuckDbType maps its
// GoogleSQL type and the precision and scale the schema gives, except that GEOGRAPHY is stored as
// its text. Stored structs still require distinct nonempty field names.
absl::StatusOr<std::string> DuckDbColumnType(const FieldSchema& field);

// The DuckDB type of a query parameter described by a BigQuery TableFieldSchema: as
// DuckDbColumnType, except that INTERVAL, a query value, is supported and GEOGRAPHY is not.
absl::StatusOr<std::string> DuckDbParameterType(const FieldSchema& field);

}  // namespace bigquery_emulator_duckdb
