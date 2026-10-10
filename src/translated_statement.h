#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "src/field_schema.h"
#include "src/references.h"
#include "src/routine.h"
#include "src/table_metadata.h"

// What the translator takes besides the resolved statement, and what it returns: the statement's
// DuckDB SQL with what the emulator records about it besides DuckDB's own catalog.

namespace bigquery_emulator_duckdb {

struct TemporaryTables;

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
  // DuckDB's CREATE TABLE AS SELECT cannot declare NOT NULL; apply it after creating the table.
  bool as_select = false;
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
  std::optional<std::optional<int64_t>> expiration_time = {};
};

// The routine a CREATE FUNCTION defines, which the emulator records next to its DuckDB macro.
struct RoutineDefinition {
  Routine routine;
  bool if_not_exists = false;
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

// ALTER TABLE ALTER COLUMN [IF EXISTS] SET OPTIONS, which sets the column's description; NULL
// clears it.
struct ColumnOptionsAction {
  std::string name;
  std::optional<std::string> description = {};
  bool if_exists = false;
};

using TableAlterAction = std::variant<AddColumnAction, DropColumnAction, RenameTableAction,
                                      ColumnOptionsAction, OptionUpdates>;

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
  // The table or view, the dataset, or the routine that a DDL statement creates, alters or drops.
  std::optional<TableReference> ddl_target_table;
  std::optional<DatasetReference> ddl_target_dataset;
  std::optional<RoutineReference> ddl_target_routine;
  // What the emulator records besides DuckDB's own catalog, at most one of which is set.
  std::optional<TableDefinition> table;
  std::optional<TableAlteration> altered_table;
  std::optional<ViewDefinition> view;
  std::optional<DatasetDefinition> dataset;
  std::optional<DatasetAlteration> altered_dataset;
  std::optional<RoutineDefinition> routine;
};

}  // namespace bigquery_emulator_duckdb
