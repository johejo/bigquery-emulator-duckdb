#pragma once

namespace duckdb {
// Keep DuckDB's C++ database headers out of the backend interface.
class DuckDB;
}  // namespace duckdb

namespace bigquery_emulator_duckdb {
void RegisterDuckPGQ(duckdb::DuckDB& database);
}  // namespace bigquery_emulator_duckdb
