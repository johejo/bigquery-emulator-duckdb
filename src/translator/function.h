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

// `value`, the SQL of a value of `type`, with each GEOGRAPHY in it, at any depth, turned into the
// WKT BigQuery returns for it, as a query returns or a table stores it. A GEOGRAPHY keeps more
// digits until then; see src/backend_functions/geography.cc.
std::string GeographyWkt(const std::string& value, const googlesql::Type* type, int depth = 0);

// `value`, the SQL of a value of `type` that a statement writes to the column `field`. DuckDB's
// cast to a parameterized NUMERIC or BIGNUMERIC rounds halfway values away from zero, so the value
// of a ROUND_HALF_EVEN field, at any depth in a STRUCT or ARRAY, is rounded to even first, as
// ROUND does. Container expressions are evaluated once and NULL STRUCTs remain NULL.
std::optional<std::string> WrittenValue(const std::string& value, const googlesql::Type* type,
                                        const FieldSchema& field);

// `value`, the SQL of a value of `type` already in the form a query returns, rounded for the
// column `field` as WrittenValue rounds it.
std::optional<std::string> RoundedToEven(const std::string& value, const googlesql::Type* type,
                                         const FieldSchema& field);

std::optional<std::string> Function(const googlesql::ResolvedFunctionCall& call, const Scope& scope,
                                    const Columns& columns);

}  // namespace bigquery_emulator_duckdb::translator
