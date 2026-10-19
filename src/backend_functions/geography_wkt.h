#pragma once

#include <string_view>

#include "absl/status/statusor.h"
#include "src/backend_functions/geography_value.h"

namespace bigquery_emulator_duckdb::backend_functions {

// The geography of WKT as ST_GEOGFROMTEXT reads it: points, linestrings and polygons, whose
// longitudes are taken modulo 360 as ST_GEOGPOINT takes them, and empty geometries. A polygon's
// rings must have four or more vertices and end where they start.
absl::StatusOr<Geography> ReadWkt(std::string_view text);

}  // namespace bigquery_emulator_duckdb::backend_functions
