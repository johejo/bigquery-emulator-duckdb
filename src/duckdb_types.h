#pragma once

// How the emulator reads the DuckDB types of query results and stored columns, which
// src/type_mapping.h spells for each GoogleSQL type.

#include <optional>
#include <string>

#include "duckdb.h"
#include "src/field_schema.h"
#include "src/timestamp_precision.h"

namespace bigquery_emulator_duckdb {

// An alias marks explicit nanosecond TIMESTAMPs; resolved types restore it at query boundaries.
bool IsTimestampNanos(duckdb_logical_type type);

// Whether the DuckDB `type` is the STRUCT that keeps a RANGE.
bool IsRange(duckdb_logical_type type);

// Whether the DuckDB `type` is the STRUCT that keeps a STRUCT<>.
bool IsEmptyStruct(duckdb_logical_type type);

// The TableFieldSchema BigQuery reports for the result column `name` of DuckDB type `type`.
FieldSchema ResultFieldSchema(const std::string& name, duckdb_logical_type type);

// The first DuckDB type in `type`, including array elements and struct fields, that the emulator
// does not support as storage, or an empty string. Only marked BIGNUM columns contain units of
// 10^-38; a plain BIGNUM is an integer, with a range larger than BIGNUMERIC's.
std::string UnsupportedDuckDbType(duckdb_logical_type type);

// Whether `type`, including array elements and struct fields, holds TIMESTAMPs of a precision
// other than `precision`, as a table created under the other shell semantics does. RANGE bounds
// keep microseconds at either precision.
bool HasOtherTimestampPrecision(duckdb_logical_type type, TimestampPrecision precision);

// A DuckDB expression that converts `sql`, of the external numeric storage type `type`, to the
// units used by all query expressions, or nullopt when the stored values already have them.
// Arrays and structs preserve NULLs, including a NULL struct rather than a struct of NULL fields.
std::optional<std::string> ReadNumericStorage(const std::string& sql, duckdb_logical_type type);

}  // namespace bigquery_emulator_duckdb
