#pragma once

#include <optional>
#include <string>

#include "src/query_parameters.h"

namespace googlesql {
class ResolvedStatement;
}

namespace bigquery_emulator_duckdb {

// Translates queries: projections, table reads, filters, ordering, limits, joins, CTEs,
// aggregation, analytic functions, set operations, UNNEST and subqueries, with supported
// expressions, and INSERT, UPDATE, DELETE and MERGE. Columns are bound by resolved ID across
// scan scopes. nullopt means an unsupported construct, allowing the caller to translate the
// whole statement through the parser AST; `unsupported`, when given, then names it.
// Errors are not caught: a failed translation or execution must not trigger fallback.
// The statement's catalog and TypeFactory must remain alive for this call.
std::optional<std::string> TranslateResolvedToDuckDbSql(
    const googlesql::ResolvedStatement& statement, const QueryParameters& parameters = {},
    std::string* unsupported = nullptr);

}  // namespace bigquery_emulator_duckdb
