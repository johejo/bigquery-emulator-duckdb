#pragma once

#include <string>
#include <vector>

#include "absl/status/statusor.h"
#include "src/field_schema.h"

namespace duckdb {
// MySQL query metadata users should not also parse DuckDB's database headers.
class DuckDB;
}  // namespace duckdb

namespace bigquery_emulator_duckdb {

// Registers the statically linked MySQL extension and its BigQuery-compatible settings.
void RegisterMySqlScanner(duckdb::DuckDB& database);

// Describes a MySQL query without executing it. conninfo is the extension's key=value DSN.
absl::StatusOr<std::vector<FieldSchema>> DescribeMySqlQuery(const std::string& conninfo,
                                                            const std::string& query,
                                                            FieldType decimal_type);

}  // namespace bigquery_emulator_duckdb
