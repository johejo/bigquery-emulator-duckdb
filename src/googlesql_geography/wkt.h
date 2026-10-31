#pragma once

#include <string_view>

#include "absl/status/statusor.h"
#include "src/googlesql_geography/value.h"

namespace googlesql_geography {

// The geography of WKT as ST_GEOGFROMTEXT reads it: points, linestrings, polygons, their
// MULTIPOINT, MULTILINESTRING and MULTIPOLYGON, and GEOMETRYCOLLECTIONs of any of these, whose
// longitudes are taken modulo 360 as ST_GEOGPOINT takes them, empty geometries and FULLGLOBE.
// Each part is built on its own and the parts are combined as Combine does. A polygon's rings
// must have four or more vertices and end where they start. With `oriented`, as loads and query
// parameters read WKT, a polygon's interior lies to the left of its rings; the emulator
// takes only polygons with a counterclockwise shell and clockwise holes, which enclose the regions
// it builds. `failed` prefixes the errors. With `planar`, edges are tessellated from unwrapped
// longitude/latitude coordinates to within 10 meters, as ReadWkb's planar edges are.
absl::StatusOr<Geography> ReadWkt(std::string_view text, bool oriented = false,
                                  std::string_view failed = "ST_GeogFromText failed: ",
                                  bool planar = false, bool make_valid = false);

}  // namespace googlesql_geography
