#pragma once

#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "googlesql/public/analyzer.h"
#include "src/catalog.h"
#include "src/field_schema.h"
#include "src/query_parameters.h"
#include "src/references.h"
#include "src/table_metadata.h"

namespace googlesql {
class ResolvedExpr;
class ResolvedStatement;
}  // namespace googlesql

namespace bigquery_emulator_duckdb {

// The project and dataset that unqualified table and dataset names in DDL belong to, and the
// temporary tables of a multi-statement query that take precedence over them, matching the
// defaults the catalog resolves queries with, plus the execution identity availability.
// `dataset` may be empty.
struct DefaultDataset {
  std::string project;
  std::string dataset;
  const TemporaryTables* temporary = nullptr;
  // Whether SESSION_USER is available from the execution backend.
  bool has_session_user = false;
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
// the description, friendly name and labels its OPTIONS give. CREATE TABLE COPY and CLONE also
// fill it with the rows of `rows_from`.
struct TableDefinition {
  TableReference table;
  std::vector<FieldSchema> schema = {};
  TableMetadata metadata;
  bool if_not_exists = false;
  std::optional<TableReference> rows_from = {};
};

// The dataset a CREATE SCHEMA defines, with the description, friendly name and labels its OPTIONS
// give, which the emulator records next to the DuckDB schema.
struct DatasetDefinition {
  DatasetReference dataset;
  DatasetMetadata metadata;
  bool if_not_exists = false;
};

// What SET OPTIONS sets on a table or dataset: each option it names replaces the value there,
// and NULL clears it.
struct OptionUpdates {
  std::optional<std::string> description = {};
  std::optional<std::string> friendly_name = {};
  std::optional<std::map<std::string, std::string>> labels = {};
};

// ALTER TABLE ADD COLUMN [IF NOT EXISTS].
struct AddColumnAction {
  FieldSchema field;
  bool if_not_exists = false;
};

// ALTER TABLE DROP COLUMN [IF EXISTS].
struct DropColumnAction {
  std::string name;
  bool if_exists = false;
};

// ALTER TABLE RENAME TO, which keeps the table in its dataset.
struct RenameTableAction {
  std::string table_id;
};

using TableAlterAction =
    std::variant<AddColumnAction, DropColumnAction, RenameTableAction, OptionUpdates>;

// The actions of an ALTER TABLE, in order. DuckDB takes one action per ALTER and keeps neither
// the BigQuery schema nor the metadata they change, so the emulator applies them to what the table
// has when the statement runs.
struct TableAlteration {
  TableReference table;
  std::vector<TableAlterAction> actions = {};
  bool if_exists = false;
};

// ALTER SCHEMA SET OPTIONS, which the emulator applies to what the dataset has when it runs.
struct DatasetAlteration {
  DatasetReference dataset;
  OptionUpdates options = {};
  bool if_exists = false;
};

// A statement translated to DuckDB, with what the emulator needs to know about it besides its SQL.
struct TranslatedStatement {
  // Empty for ALTER TABLE and ALTER SCHEMA, whose DuckDB statements depend on what the table or
  // dataset has when they run.
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
  std::optional<TableAlteration> altered_table;
  std::optional<ViewDefinition> view;
  std::optional<DatasetDefinition> dataset;
  std::optional<DatasetAlteration> altered_dataset;
};

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
    const DefaultDataset& defaults = {}, std::string* unsupported = nullptr,
    const googlesql::SystemVariableValuesMap* system_variables = nullptr);

// Translates the expression of a script, such as a variable's DEFAULT or an IF condition, into a
// DuckDB query returning its value as one row of one column, like TranslateStatement does.
std::optional<std::string> TranslateExpression(
    const googlesql::ResolvedExpr& expression, const QueryParameters& parameters,
    const DefaultDataset& defaults, std::string* unsupported,
    const googlesql::SystemVariableValuesMap* system_variables);

}  // namespace bigquery_emulator_duckdb
