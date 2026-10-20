#pragma once

#include <string>
#include <unordered_map>

// OBJ.FETCH_METADATA over the objects that GcsClient reads.

namespace bigquery_emulator_duckdb {

class GcsClient;

// The JSON of the ObjectRef that OBJ.FETCH_METADATA returns for the JSON `objectref`, in a query
// of `project`. `connections` maps each connection's PROJECT.LOCATION.CONNECTION to what
// --connection gives it; only Cloud resource connections authorize reads. As in BigQuery, a
// failure is recorded in the details rather than thrown.
std::string FetchObjectMetadata(GcsClient& gcs,
                                const std::unordered_map<std::string, std::string>& connections,
                                const std::string& objectref, const std::string& project);

}  // namespace bigquery_emulator_duckdb
