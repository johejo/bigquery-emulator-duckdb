#pragma once

#include <optional>
#include <string>

#include "src/translator/context.h"

namespace googlesql {
class ResolvedFunctionCall;
class ResolvedFunctionCallBase;
class Type;
}  // namespace googlesql

namespace bigquery_emulator_duckdb {
struct FieldSchema;
}  // namespace bigquery_emulator_duckdb

namespace bigquery_emulator_duckdb::translator {

// Scalar functions and operators, in function.cc.

// Whether `type` is BIGNUMERIC or holds one, at any depth.
bool HasBigNumeric(const googlesql::Type* type);

// Whether the result or an argument of `call` has a BIGNUMERIC, at any depth.
bool InvolvesBigNumeric(const googlesql::ResolvedFunctionCallBase& call);

// `value`, the SQL of a value of `type` that a statement writes to the column `field`. DuckDB's
// cast to a parameterized NUMERIC or BIGNUMERIC rounds halfway values away from zero, so the value
// of a ROUND_HALF_EVEN column is rounded to even first, as ROUND does. Writing a STRUCT or ARRAY
// with such values is unsupported.
std::optional<std::string> WrittenValue(std::string value, const googlesql::Type* type,
                                        const FieldSchema& field, const Scope& scope);

std::optional<std::string> Function(const googlesql::ResolvedFunctionCall& call, const Scope& scope,
                                    const Columns& columns);

}  // namespace bigquery_emulator_duckdb::translator
