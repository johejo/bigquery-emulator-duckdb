#pragma once

#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "src/query_parameters.h"
#include "src/translated_statement.h"

namespace googlesql {
class ResolvedExpr;
class ResolvedStatement;
class Value;
struct StringVectorCaseLess;
// As googlesql/public/analyzer.h declares it, which takes seconds to parse.
using SystemVariableValuesMap = std::map<std::vector<std::string>, Value, StringVectorCaseLess>;
}  // namespace googlesql

namespace bigquery_emulator_duckdb {

// Translates queries: projections, table reads, filters, ordering, limits, joins, CTEs,
// aggregation, analytic functions, set operations, UNNEST and subqueries, with supported
// expressions, INSERT, UPDATE, DELETE and MERGE, and CREATE TABLE [AS SELECT], CREATE SCHEMA,
// CREATE VIEW, ALTER TABLE, ALTER SCHEMA and DROP TABLE/VIEW/SCHEMA. Columns are bound by resolved
// ID across scan scopes. nullopt means an unsupported construct, which `unsupported`, when given,
// then names. Errors are not caught, so that a failed translation is not reported as an unsupported
// one. The statement's catalog and TypeFactory must remain alive for this call. A statement of a
// script reads the script's variables as catalog constants and its system variables from
// `system_variables`.
std::optional<TranslatedStatement> TranslateStatement(
    const googlesql::ResolvedStatement& statement, const QueryParameters& parameters = {},
    // LLVM 23 mistakes the argument separator after {} for an initializer's trailing comma.
    // NOLINTNEXTLINE(readability-trailing-comma)
    const DefaultDataset& defaults = {}, std::string* unsupported = nullptr,
    const googlesql::SystemVariableValuesMap* system_variables = nullptr);

// Translates the expression of a script, such as a variable's DEFAULT or an IF condition, into a
// DuckDB query returning its value as one row of one column, like TranslateStatement does.
std::optional<std::string> TranslateExpression(
    const googlesql::ResolvedExpr& expression, const QueryParameters& parameters,
    const DefaultDataset& defaults, std::string* unsupported,
    const googlesql::SystemVariableValuesMap* system_variables);

// The query parameters that EXECUTE IMMEDIATE passes with USING, as literals of their values.
// nullopt means a value the emulator cannot write as a literal, which `unsupported` then names.
std::optional<QueryParameters> TranslateParameters(
    const std::variant<std::vector<googlesql::Value>, std::map<std::string, googlesql::Value>>&
        values,
    std::string* unsupported);

}  // namespace bigquery_emulator_duckdb
