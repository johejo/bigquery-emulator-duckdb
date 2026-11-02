#pragma once

#include <string_view>
#include <vector>

#include "src/field_schema.h"
#include "src/file_read.h"

namespace bigquery_emulator_duckdb {

// The schema BigQuery detects for the NEWLINE_DELIMITED_JSON `source` of the external table
// `table_id`: an object is a RECORD, an array REPEATED, a string the type DetectValue gives it,
// and a field with only nulls or empty arrays a STRING. Values of a field merge as MergeKinds
// does, and an object or an array outweighs a scalar, so that reads of the scalar fail. The fields
// of each object come in the order UnorderedMapOrder gives their names in order of appearance.
// Throws ApiError for a row that is not an object, and for names BigQuery rejects.
std::vector<FieldSchema> DetectJson(const FileSource& source, std::string_view table_id);

}  // namespace bigquery_emulator_duckdb
