#pragma once

#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "absl/status/statusor.h"
#include "src/field_schema.h"

// EXTERNAL_QUERY reads from PostgreSQL, MySQL or Spanner databases that --connection names.

namespace bigquery_emulator_duckdb {

class Backend;

// The connections that EXTERNAL_QUERY reaches, by key, and those with table macros registered
// in the DuckDB database of `backend`.
struct ExternalConnections {
  Backend& backend;
  std::unordered_map<std::string, std::string> conninfo = {};
  std::mutex mutex = {};
  std::unordered_set<std::string> attached = {};
};

// A connection that EXTERNAL_QUERY can name, and the external database it reaches.
struct ExternalConnection {
  // The connection's PROJECT.LOCATION.CONNECTION, as ConnectionKey gives it.
  std::string key;
  // A libpq connection string or URI, mysql:key=value DSN, or
  // spanner://projects/P/instances/I/databases/D.
  std::string conninfo;
};

// What --connection gives a Cloud resource connection in place of a database, which OBJ functions
// read Cloud Storage objects through and EXTERNAL_QUERY rejects.
inline constexpr std::string_view kCloudResourceConnection = "cloud_resource";

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

// The type that EXTERNAL_QUERY's options give PostgreSQL's NUMERIC and MySQL's DECIMAL: NUMERIC
// unless default_type_for_decimal_columns names another. An error status for options it rejects.
absl::StatusOr<FieldType> DecimalColumnType(const std::string& options);

// The columns that `query` returns on the PostgreSQL database `conninfo`, with the BigQuery types
// EXTERNAL_QUERY gives them, NUMERIC columns taking `decimal_type`. An error status when the
// database cannot prepare the query or a column has a type that EXTERNAL_QUERY rejects.
absl::StatusOr<std::vector<FieldSchema>> DescribePostgresQuery(const std::string& conninfo,
                                                               const std::string& query,
                                                               FieldType decimal_type);

}  // namespace bigquery_emulator_duckdb
