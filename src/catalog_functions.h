#pragma once

namespace duckdb {
// Catalog registration must not expose DuckDB's C++ headers to its callers.
class DuckDB;
}  // namespace duckdb

namespace bigquery_emulator_duckdb {

// Registers table functions that read relation names and comments from DuckDB's catalog without
// computing what duckdb_tables(), duckdb_views() and duckdb_columns() also return, such as each
// table's storage information and CREATE statement. Given a database, schema and name, they look
// the relation up instead of scanning the catalog:
//
// - bq_relations(database := ..., schema := ..., name := ...) returns database_name, schema_name,
//   table_name, type ('TABLE' or 'VIEW'), comment and table_oid for each table and view. Each named
//   argument is optional and matches the stored name exactly.
// - bq_schemas(database := ..., schema := ...) returns database_name and schema_name for each
//   schema, with the same optional arguments.
// - bq_column_comments(database, schema, name) returns column_index, from 1, and comment for each
//   column of that table or view, or no rows when there is none.
void RegisterCatalogFunctions(const duckdb::DuckDB& database);

}  // namespace bigquery_emulator_duckdb
