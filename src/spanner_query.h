#pragma once

#include <string>
#include <vector>

#include "absl/status/statusor.h"
#include "duckdb.h"
#include "src/field_schema.h"

namespace bigquery_emulator_duckdb {

// Uses Spanner's PLAN mode to resolve the result columns without executing the query.
absl::StatusOr<std::vector<FieldSchema>> DescribeSpannerQuery(const std::string& database,
                                                              const std::string& query,
                                                              const std::string& options);

// Registers the streaming table function used by Spanner EXTERNAL_QUERY connections.
void RegisterSpannerQuery(duckdb_connection connection);

}  // namespace bigquery_emulator_duckdb
