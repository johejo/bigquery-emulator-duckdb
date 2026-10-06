#pragma once

#include <optional>
#include <string>
#include <vector>

#include "googlesql/public/analyzer.h"
#include "src/field_schema.h"
#include "src/query_parameters.h"
#include "src/references.h"
#include "src/table_metadata.h"

namespace googlesql {
class ResolvedExpr;
class ResolvedStatement;
}  // namespace googlesql

namespace bigquery_emulator_duckdb {

// The project and dataset that unqualified table and dataset names in DDL belong to, matching
// the defaults the catalog resolves queries with. `dataset` may be empty.
struct DefaultDataset {
  std::string project;
  std::string dataset;
};

// The view a CREATE VIEW defines, which the emulator records next to the DuckDB view: DuckDB
// keeps neither the GoogleSQL query, its BigQuery schema nor the metadata its OPTIONS give.
struct ViewDefinition {
  TableReference table;
  std::string query;
  std::vector<FieldSchema> schema = {};
  TableMetadata metadata;
  bool if_not_exists = false;
};

// The table a CREATE TABLE [AS SELECT] defines, whose BigQuery schema the emulator records with
// it: the declared types, NOT NULL as REQUIRED, type parameters, defaults and descriptions, and
// the description, friendly name and labels its OPTIONS give.
struct TableDefinition {
  TableReference table;
  std::vector<FieldSchema> schema = {};
  TableMetadata metadata;
  bool if_not_exists = false;
};

// The dataset a CREATE SCHEMA defines, with the description, friendly name and labels its OPTIONS
// give, which the emulator records next to the DuckDB schema.
struct DatasetDefinition {
  DatasetReference dataset;
  DatasetMetadata metadata;
  bool if_not_exists = false;
};

// The column an ALTER TABLE ADD COLUMN adds.
struct AddedColumn {
  TableReference table;
  FieldSchema field;
  bool if_table_exists = false;
  bool if_column_not_exists = false;
};

// A statement translated to DuckDB, with what the emulator needs to know about it besides its SQL.
struct TranslatedStatement {
  std::string sql;
  // JobStatistics2.statementType: SELECT, INSERT, CREATE_TABLE_AS_SELECT, DROP_VIEW, ...
  std::string statement_type;
  // The schema of the rows a query returns, as ResultSchema reports it; empty for other statements.
  std::optional<std::vector<FieldSchema>> result_schema;
  // The table or view, or the dataset, that a DDL statement creates, alters or drops.
  std::optional<TableReference> ddl_target_table;
  std::optional<DatasetReference> ddl_target_dataset;
  // What the emulator records besides DuckDB's own catalog, at most one of which is set.
  std::optional<TableDefinition> table;
  std::optional<AddedColumn> added_column;
  std::optional<ViewDefinition> view;
  std::optional<DatasetDefinition> dataset;
};

// Translates queries: projections, table reads, filters, ordering, limits, joins, CTEs,
// aggregation, analytic functions, set operations, UNNEST and subqueries, with supported
// expressions, INSERT, UPDATE, DELETE and MERGE, and CREATE TABLE [AS SELECT], CREATE SCHEMA
// CREATE VIEW and DROP TABLE/VIEW/SCHEMA. Columns are bound by resolved ID across scan scopes.
// nullopt means an unsupported construct, which `unsupported`, when given, then names. Errors are
// not caught, so that a failed translation is not reported as an unsupported one. The statement's
// catalog and TypeFactory must remain alive for this call. A statement of a script reads the
// script's variables as catalog constants and its system variables from `system_variables`.
std::optional<TranslatedStatement> TranslateStatement(
    const googlesql::ResolvedStatement& statement, const QueryParameters& parameters = {},
    const DefaultDataset& defaults = {}, std::string* unsupported = nullptr,
    const googlesql::SystemVariableValuesMap* system_variables = nullptr);

// Translates the expression of a script, such as a variable's DEFAULT or an IF condition, into a
// DuckDB query returning its value as one row of one column, like TranslateStatement does.
std::optional<std::string> TranslateExpression(
    const googlesql::ResolvedExpr& expression, const QueryParameters& parameters,
    const DefaultDataset& defaults, std::string* unsupported,
    const googlesql::SystemVariableValuesMap* system_variables);

}  // namespace bigquery_emulator_duckdb
