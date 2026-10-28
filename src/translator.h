#pragma once

#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "src/default_dataset.h"
#include "src/query_parameters.h"
#include "src/system_variables.h"
#include "src/translated_statement.h"

namespace googlesql {
class ResolvedExpr;
class ResolvedStatement;
class Value;
}  // namespace googlesql

namespace bigquery_emulator_duckdb::function_catalog {
class Catalog;
}

namespace bigquery_emulator_duckdb {

struct FieldSchema;

// Translates queries: projections, table reads, filters, ordering, limits, joins, CTEs,
// aggregation, analytic functions, set operations, UNNEST and subqueries, with supported
// expressions, INSERT, UPDATE, DELETE and MERGE, and CREATE TABLE [AS SELECT], CREATE SCHEMA,
// CREATE VIEW, ALTER TABLE, ALTER SCHEMA and DROP TABLE/VIEW/SCHEMA. Columns are bound by resolved
// ID across scan scopes. nullopt means an unsupported construct, which `unsupported`, when given,
// then names. Errors are not caught, so that a failed translation is not reported as an unsupported
// one. The statement's catalog and TypeFactory must remain alive for this call. A statement of a
// script reads the script's variables as catalog constants and its system variables from
// `system_variables`. Resource-backed reads prepare after analysis using defaults.resources,
// which the returned statement retains through execution; callers retain it with returned rows.
// Without an owner such reads are unsupported, as they are in stored SQL definitions.
std::optional<TranslatedStatement> TranslateStatement(
    const function_catalog::Catalog& functions, const googlesql::ResolvedStatement& statement,
    const QueryParameters& parameters = {},
    // LLVM 23 mistakes the argument separator after {} for an initializer's trailing comma.
    // NOLINTNEXTLINE(readability-trailing-comma)
    const DefaultDataset& defaults = {}, std::string* unsupported = nullptr,
    const googlesql::SystemVariableValuesMap* system_variables = nullptr);

// Translates the expression of a script, such as a variable's DEFAULT or an IF condition, into a
// DuckDB query returning its value as one row of one column, like TranslateStatement does.
// The caller must keep defaults.resources alive through execution and consumption of the value.
std::optional<std::string> TranslateExpression(
    const function_catalog::Catalog& functions, const googlesql::ResolvedExpr& expression,
    const QueryParameters& parameters, const DefaultDataset& defaults, std::string* unsupported,
    const googlesql::SystemVariableValuesMap* system_variables);

// The query parameters that EXECUTE IMMEDIATE passes with USING, as literals of their values.
// nullopt means a value the emulator cannot write as a literal, which `unsupported` then names.
std::optional<QueryParameters> TranslateParameters(
    const std::variant<std::vector<googlesql::Value>, std::map<std::string, googlesql::Value>>&
        values,
    std::string* unsupported);

// `value`, the SQL of a column of `source` that a job appends to the existing column
// `destination`, with the values of its ROUND_HALF_EVEN fields rounded to their scale as DML
// rounds them, and its REQUIRED and length constraints checked. DuckDB's casts round halfway values
// away from zero. nullopt when the emulator cannot round them, such as values that are not NUMERIC
// or BIGNUMERIC or STRUCT fields in another order.
std::optional<std::string> TranslateAppendedValue(const function_catalog::Catalog& functions,
                                                  const std::string& value,
                                                  const FieldSchema& source,
                                                  const FieldSchema& destination);

}  // namespace bigquery_emulator_duckdb
