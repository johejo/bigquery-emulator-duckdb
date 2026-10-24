#pragma once

#include <string>
#include <string_view>

#include "absl/status/statusor.h"
#include "src/googlesql_s2geometry/value.h"

namespace bigquery_emulator_duckdb::geography_duckdb {

// DuckDB query values and stored columns use XYZ text with round-trippable doubles,
// S2 polygon rings, GEOMETRYCOLLECTION EMPTY, and fullglobe. This is not external WKT.
std::string GeographyText(const googlesql_s2geometry::Geography& geography);
absl::StatusOr<googlesql_s2geometry::Geography> ParseGeography(std::string_view text);

}  // namespace bigquery_emulator_duckdb::geography_duckdb
