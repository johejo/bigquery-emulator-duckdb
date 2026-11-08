#pragma once

namespace duckdb {
// Keep extension and database headers out of the service interface.
class DuckDB;
}  // namespace duckdb

namespace bigquery_emulator_duckdb {

// Registers DuckDB's postgres_scanner extension, which is linked into the binary, in `database`.
// Throws BackendError when DuckDB rejects it.
void RegisterPostgresScanner(duckdb::DuckDB& database);

}  // namespace bigquery_emulator_duckdb
