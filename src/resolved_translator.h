#pragma once

#include <optional>
#include <string>

#include "src/query_parameters.h"

namespace googlesql {
class ResolvedStatement;
}

namespace bigquery_emulator_duckdb {

// The project and dataset that unqualified table and dataset names in DDL belong to, matching
// the defaults the catalog resolves queries with. `dataset` may be empty.
struct DefaultDataset {
  std::string project;
  std::string dataset;
};

// Translates queries: projections, table reads, filters, ordering, limits, joins, CTEs,
// aggregation, analytic functions, set operations, UNNEST and subqueries, with supported
// expressions, INSERT, UPDATE, DELETE and MERGE, and CREATE TABLE [AS SELECT], CREATE SCHEMA
// and DROP TABLE/SCHEMA. Columns are bound by resolved ID across scan scopes. nullopt means an
// unsupported construct, allowing the caller to translate the whole statement through the
// parser AST; `unsupported`, when given, then names it.
// Errors are not caught: a failed translation or execution must not trigger fallback.
// The statement's catalog and TypeFactory must remain alive for this call.
std::optional<std::string> TranslateResolvedToDuckDbSql(
    const googlesql::ResolvedStatement& statement, const QueryParameters& parameters = {},
    const DefaultDataset& defaults = {}, std::string* unsupported = nullptr);

}  // namespace bigquery_emulator_duckdb
