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
// CREATE VIEW and DROP TABLE/VIEW/SCHEMA. Columns are bound by resolved ID across scan scopes.
// nullopt means an unsupported construct, which `unsupported`, when given, then names. Errors are
// not caught, so that a failed translation is not reported as an unsupported one. The statement's
// catalog and TypeFactory must remain alive for this call.
std::optional<std::string> TranslateToDuckDbSql(const googlesql::ResolvedStatement& statement,
                                                const QueryParameters& parameters = {},
                                                const DefaultDataset& defaults = {},
                                                std::string* unsupported = nullptr);

}  // namespace bigquery_emulator_duckdb
