#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

#include "absl/status/statusor.h"
#include "src/googlesql_geography/value.h"

namespace googlesql_geography {

// A point, a linestring or a polygon as WKT or WKB gives it, before it is built.
struct Part {
  enum class Type : uint8_t { kPoint, kLineString, kPolygon, kFull };
  Type type;
  // A polygon's rings, or the vertices of a point or a linestring.
  std::vector<std::vector<Point>> rings;
};

// The geography of `parts` as ST_GEOGFROMTEXT and ST_GEOGFROMWKB build them: latitudes must be
// in range and longitudes are taken modulo 360 as ST_GEOGPOINT takes them, each part is built on
// its own and the parts are combined as Combine does. A linestring needs two or more vertices and
// a polygon's rings four or more that end where they start. With `oriented`, a polygon's interior
// lies to the left of its rings; the emulator takes only polygons with a counterclockwise shell
// and clockwise holes, which enclose the regions it builds. `failed` prefixes the errors.
absl::StatusOr<Geography> BuildParts(std::vector<Part> parts, bool oriented,
                                     std::string_view failed);

// Build WKB's decoded coordinates with planar rather than geodesic edges.
absl::StatusOr<Geography> BuildPlanarParts(const std::vector<Part>& parts, std::string_view failed);

}  // namespace googlesql_geography
