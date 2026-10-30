#pragma once

#include <optional>
#include <string>
#include <string_view>

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

// `value`, the SQL of a value of `type`, as a query returns it in the column `column`, or with
// `stored` as a table stores it: an ARRAY with a NULL element fails where `functions` has
// BigQuery's restriction, and unless `stored`, each GEOGRAPHY in it, at any depth, is turned into
// the WKT BigQuery returns for it.
// Until then, an ARRAY may hold NULL, and a GEOGRAPHY, also in tables, keeps the form queries hold
// it in (see src/googlesql_geography_duckdb/codec.h).
std::string ResultValue(const function_catalog::Catalog& functions, const std::string& value,
                        const googlesql::Type* type, std::string_view column, bool stored);

// `value`, the SQL of a value of `type` that a statement writes to the column `field`. DuckDB's
// cast to a parameterized NUMERIC or BIGNUMERIC rounds halfway values away from zero, so the value
// of a ROUND_HALF_EVEN field, at any depth in a STRUCT or ARRAY, is rounded to even first, as
// ROUND does. REQUIRED and length constraints are checked at every depth. Container expressions are
// evaluated once and NULL STRUCTs remain NULL.
std::optional<std::string> WrittenValue(const function_catalog::Catalog& functions,
                                        const std::string& value, const googlesql::Type* type,
                                        const FieldSchema& field, bool assignment = false);

// `value`, the SQL of a value of `type` already in the form a query returns, rounded for the
// column `field` as WrittenValue rounds it.
std::optional<std::string> RoundedToEven(const function_catalog::Catalog& functions,
                                         const std::string& value, const googlesql::Type* type,
                                         const FieldSchema& field);

std::optional<std::string> Function(const googlesql::ResolvedFunctionCall& call, const Scope& scope,
                                    const Columns& columns);

}  // namespace bigquery_emulator_duckdb::translator
