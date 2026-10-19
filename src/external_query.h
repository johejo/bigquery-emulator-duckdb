#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "absl/status/statusor.h"
#include "src/field_schema.h"

// EXTERNAL_QUERY reads from PostgreSQL or Spanner databases that --connection names.

namespace bigquery_emulator_duckdb {

// A connection that EXTERNAL_QUERY can name, and the external database it reaches.
struct ExternalConnection {
  // The connection's PROJECT.LOCATION.CONNECTION, as ConnectionKey gives it.
  std::string key;
  // A libpq connection string or URI, or spanner://projects/P/instances/I/databases/D.
  std::string conninfo;
};

// Parses --connection's PROJECT.LOCATION.CONNECTION=CONNINFO. Throws std::invalid_argument.
ExternalConnection ParseConnectionArgument(std::string_view argument);

// The PROJECT.LOCATION.CONNECTION of the connection that `connection_id` names, with the location
// in lower case, or nothing for an ID of another form. EXTERNAL_QUERY accepts
// projects/PROJECT/locations/LOCATION/connections/CONNECTION, PROJECT.LOCATION.CONNECTION and
// LOCATION.CONNECTION, which belongs to `default_project` when it is not empty.
std::optional<std::string> ConnectionKey(std::string_view connection_id,
                                         const std::string& default_project);

// The internal DuckDB attachment and table macro name, which a project ID cannot name.
std::string ExternalDatabaseName(const std::string& key);

// The type that EXTERNAL_QUERY's options give PostgreSQL's NUMERIC: NUMERIC unless
// default_type_for_decimal_columns names another. An error status for options it rejects.
absl::StatusOr<FieldType> DecimalColumnType(const std::string& options);

// The columns that `query` returns on the PostgreSQL database `conninfo`, with the BigQuery types
// EXTERNAL_QUERY gives them, NUMERIC columns taking `decimal_type`. An error status when the
// database cannot prepare the query or a column has a type that EXTERNAL_QUERY rejects.
absl::StatusOr<std::vector<FieldSchema>> DescribePostgresQuery(const std::string& conninfo,
                                                               const std::string& query,
                                                               FieldType decimal_type);

}  // namespace bigquery_emulator_duckdb
