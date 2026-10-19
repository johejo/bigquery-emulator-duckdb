#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "s2/s2point.h"
#include "s2/s2polygon.h"
#include "s2/s2shape.h"

namespace bigquery_emulator_duckdb::backend_functions {

// A GEOGRAPHY holds S2Points, as BigQuery's do: POINT(x y z), LINESTRING(x y z, ...) or
// POLYGON((x y z, ...), ...) with the shortest digits that round-trip, or GEOMETRYCOLLECTION
// EMPTY. Coordinates are those of
// S2LatLng, which gives ST_X(ST_GEOGPOINT(0.1 + 0.2, 1)) 0.3000000000000001 as BigQuery does. The
// translator turns a GEOGRAPHY into WKT with bq_st_astext wherever a query returns or stores one.
struct Point {
  double longitude;
  double latitude;
};

enum class Kind : uint8_t {
  // The empty geography, which has no geometry type.
  kEmpty,
  kPoint,
  kLineString,
  kPolygon,
};

// A geography and its vertices: none for the empty geography, one for a point and two or more for
// a linestring. A polygon has no vertices but loops, its shell and then its holes, each as
// S2Polygon holds it: counterclockwise without repeating its first vertex, so that the shell
// encloses at most half the sphere and a hole is the region it removes. EachRow handles SQL
// NULLs.
struct Geography {
  Kind kind = Kind::kEmpty;
  std::vector<S2Point> vertices;
  std::vector<std::vector<S2Point>> loops;
};

Geography PointGeography(const S2Point& point);

// The shortest digits that round-trip, or with `precision` 15 significant digits, as printf's
// %.15g writes them and BigQuery writes 0.1 + 0.2 as 0.3 and 0.0000001 as 1e-07. BigQuery writes
// -0 as 0.
std::string Coordinate(double value, std::optional<int> precision = 15);

S2Point ToS2Point(const Point& point);
Point ToPoint(const S2Point& point);

// The WKT BigQuery returns for `geography`, whose coordinates have 15 significant digits. A
// polygon's rings repeat their first vertex, and a hole's run clockwise, as S2Polygon's shape
// walks it.
std::string Wkt(const Geography& geography);

// The GEOGRAPHY of `geography`.
std::string GeographyText(const Geography& geography);

// Parses a GEOGRAPHY as GeographyText writes it.
absl::StatusOr<Geography> ParseGeography(std::string_view text);

// The shape of a nonempty geography.
std::unique_ptr<S2Shape> Shape(const Geography& geography);

// The S2Polygon of a polygon.
std::unique_ptr<S2Polygon> ToS2Polygon(const Geography& polygon);

// The same point as BigQuery's ST_EQUALS and ST_AZIMUTH take it, within a micrometer in every
// direction: POINT(0.30000000000000004 0) is POINT(0.3 0), though their distance is not 0, and a
// pole is the same at every longitude.
bool Same(const S2Point& a, const S2Point& b);

// BigQuery takes points as antipodal only where their S2Points are exactly opposite, as
// POINT(-30 0) and POINT(150 0) are and POINT(10 20) and POINT(-170 -20) are not.
bool Antipodal(const S2Point& p, const S2Point& q);

// "point" or "points" after `count`, as BigQuery's errors write it.
std::string Points(size_t count);

// Fails where consecutive `vertices` are antipodal, so that their edge has no direction: their
// S2Points are opposite, or their coordinates are, as BigQuery takes LINESTRING(0 0, 180 0) and
// the poles, whose S2Points are not. `failed` prefixes the error.
absl::Status CheckAntipodal(const std::vector<Point>& vertices, std::string_view failed);

// The linestring of `vertices`, two or more without antipodal neighbors, as BigQuery builds it
// with S2Builder: vertices within a micrometer are snapped together, a line folding back on itself
// merges with itself, and crossing edges are split where they cross. A long edge gains its
// midpoint, and a line that snaps to one point is the point S2Builder snaps it to. The emulator
// does not hold the MULTILINESTRING a line that overlaps itself becomes.
absl::StatusOr<Geography> BuildLine(const std::vector<S2Point>& vertices);

// The polygon of `rings`, a shell and then its holes, as BigQuery builds one where its
// orientation is not given: a vertex that repeats the one before it is dropped, an edge longer
// than 135 degrees gains its midpoint as a linestring's does, and each ring encloses the smaller of
// the regions it divides the sphere into, which a hole removes from the shell's. Rings must have
// three or more distinct vertices and not cross, and holes must lie inside the shell and outside
// each other; `failed` prefixes the error. Once each ring is valid, vertices within a micrometer
// of each other snap to the one S2Builder keeps: a hole that collapses is dropped, and a shell
// without holes that collapses is the linestring or the point it leaves. BigQuery also snaps edges,
// and rings where they cross or touch at an edge, which is unsupported, as is a shell with holes
// that collapses.
absl::StatusOr<Geography> BuildPolygon(const std::vector<std::vector<S2Point>>& input,
                                       std::string_view failed);

}  // namespace bigquery_emulator_duckdb::backend_functions
