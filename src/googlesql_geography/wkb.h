#pragma once

#include <string>
#include <string_view>

#include "absl/status/statusor.h"
#include "src/googlesql_geography/value.h"

namespace googlesql_geography {

// The geography of WKB as ST_GEOGFROMWKB reads it, in either byte order: the 2D points,
// linestrings, polygons, their multipart types and collections that WKT gives, built as ReadWkt
// builds them, where a point whose coordinates are both NaN is empty. Z and M coordinates are
// errors, and EWKB's SRID must be WGS84's. With `oriented`, as loads read WKB, polygons are taken
// as ReadWkt takes them. `failed` prefixes the errors, which end saying whether parsing or the
// geography failed, as BigQuery's do.
absl::StatusOr<Geography> ReadWkb(std::string_view bytes,
                                  std::string_view failed = "ST_GeogFromWKB failed: ",
                                  bool oriented = false);

// The little-endian WKB of `geography`: one point, linestring or polygon, a MULTIPOINT,
// MULTILINESTRING or MULTIPOLYGON of more than one, and a GEOMETRYCOLLECTION of each of them where
// it has parts of more than one dimension, with the coordinates that ToPoint gives.
std::string Wkb(const Geography& geography);

}  // namespace googlesql_geography
