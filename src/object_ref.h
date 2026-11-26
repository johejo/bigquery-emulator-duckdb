#pragma once

#include <string>
#include <unordered_map>

#include "src/gcs.h"

// OBJ.FETCH_METADATA over the objects that GcsClient reads.

namespace bigquery_emulator_duckdb {

// The JSON details of the ObjectRef of an object whose current version has `metadata`, as
// OBJ.FETCH_METADATA and object tables give them: {"gcs_metadata": {...}}. OBJ.FETCH_METADATA
// gives the time of the last update in whole seconds, and an object table's `ref` in
// microseconds: object_tables_followups.api.yaml.
std::string ObjectRefDetails(const GcsObjectMetadata& metadata, bool whole_seconds);

// The JSON of the ObjectRef that OBJ.FETCH_METADATA returns for the JSON `objectref`, in a query
// of `project`. `connections` maps each connection's PROJECT.LOCATION.CONNECTION to what
// --connection gives it; only Cloud resource connections authorize reads. As in BigQuery, a
// failure is recorded in the details rather than thrown.
std::string FetchObjectMetadata(GcsClient& gcs,
                                const std::unordered_map<std::string, std::string>& connections,
                                const std::string& objectref, const std::string& project);

}  // namespace bigquery_emulator_duckdb
