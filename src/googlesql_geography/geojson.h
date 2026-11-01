#pragma once

#include <string_view>

#include "absl/status/statusor.h"
#include "src/googlesql_geography/value.h"

namespace googlesql_geography {

// The prefix of ReadGeoJson's errors.
inline constexpr std::string_view kGeoJsonFailed = "ST_GeogFromGeoJSON failed: ";

// The geography of a GeoJSON geometry object as ST_GEOGFROMGEOJSON reads it: a Point, MultiPoint,
// LineString, MultiLineString, Polygon, MultiPolygon or a GeometryCollection of these, with
// types in any case and no other members, whose positions are a longitude and a latitude. Edges
// are planar, so each gains the vertices S2EdgeTessellator adds to keep the geodesic edges within
// 10 meters of it, and an edge between longitudes 170 and -170 runs through 0. The parts are
// combined as Combine does. A polygon is the region its rings enclose on the plane, whichever way
// they run, however large; one with an edge along a pole is unsupported.
absl::StatusOr<Geography> ReadGeoJson(std::string_view text, bool make_valid = false);

}  // namespace googlesql_geography
