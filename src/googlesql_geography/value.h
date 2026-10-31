#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "s2/mutable_s2shape_index.h"
#include "s2/r2.h"
#include "s2/s2boolean_operation.h"
#include "s2/s2buffer_operation.h"
#include "s2/s2builder.h"
#include "s2/s2latlng.h"
#include "s2/s2point.h"
#include "s2/s2polygon.h"
#include "s2/s2projections.h"
#include "s2/s2shape.h"
#include "s2/s2shape_index.h"

namespace googlesql_geography {

// Coordinates in degrees, which ToPoint and ToS2Point convert to and from S2Point.
struct Point {
  double longitude;
  double latitude;
};

// A polygon's loops, its shell and then its holes, each as S2Polygon holds it: counterclockwise
// without repeating its first vertex. A shell can enclose more than half the sphere; a hole is
// the region it removes. The full polygon has S2Loop::kFull() as its only ring, a single
// sentinel vertex that is not part of its boundary.
using Polygon = std::vector<std::vector<S2Point>>;

// A geography as BigQuery holds it, the union of its points, linestrings of two or more vertices
// and polygons, which Combine makes of any parts; none of them makes the empty geography. SQL NULLs
// are handled by the caller.
struct Geography {
  std::vector<S2Point> points;
  std::vector<std::vector<S2Point>> lines;
  std::vector<Polygon> polygons;

  [[nodiscard]] bool Empty() const { return points.empty() && lines.empty() && polygons.empty(); }
  // The number of points, linestrings and polygons.
  [[nodiscard]] size_t Count() const { return points.size() + lines.size() + polygons.size(); }
  [[nodiscard]] bool Full() const {
    return polygons.size() == 1 && polygons.front().size() == 1 &&
           polygons.front().front().size() == 1;
  }
};

Geography PointGeography(const S2Point& point);
Geography FullGeography();

// The shortest digits that round-trip, or with `precision` 15 significant digits, as printf's
// %.15g writes them and BigQuery writes 0.1 + 0.2 as 0.3 and 0.0000001 as 1e-07. BigQuery writes
// -0 as 0.
std::string Coordinate(double value, std::optional<int> precision = 15);

// The conversions of S2LatLng, but with correctly rounded atan2, sin and cos, as BigQuery's
// coordinates show, rather than the platform's math library, whose last bit can differ.
S2Point ToS2Point(const Point& point);
S2LatLng ToLatLng(const S2Point& point);
Point ToPoint(const S2Point& point);

// The plate carrée projection in degrees, converting as ToPoint and ToS2Point do.
class DegreesProjection : public S2::Projection {
 public:
  [[nodiscard]] R2Point Project(const S2Point& p) const override;
  [[nodiscard]] S2Point Unproject(const R2Point& p) const override;
  [[nodiscard]] R2Point FromLatLng(const S2LatLng& ll) const override {
    return plate_.FromLatLng(ll);
  }
  [[nodiscard]] S2LatLng ToLatLng(const R2Point& p) const override { return plate_.ToLatLng(p); }
  [[nodiscard]] R2Point wrap_distance() const override { return plate_.wrap_distance(); }

 private:
  S2::PlateCarreeProjection plate_{180};
};

// The WKT BigQuery returns for `geography`, whose coordinates have 15 significant digits: one
// point, linestring or polygon, a MULTIPOINT, MULTILINESTRING or MULTIPOLYGON of more than one,
// and a GEOMETRYCOLLECTION of these where it has parts of more than one dimension. A polygon's
// rings repeat their first vertex, and a hole's run clockwise, as S2Polygon's shape walks it.
std::string Wkt(const Geography& geography);

// Adds the shapes of `geography` to `index`: its points, each linestring and its polygons.
void AddShapes(const Geography& geography, MutableS2ShapeIndex& index);

// The number of shapes AddShapes adds for `geography`, and the one of them it adds with `id`.
int ShapeCount(const Geography& geography);
std::unique_ptr<S2Shape> Shape(const Geography& geography, int id);

// A geography with an index of the shapes AddShapes adds for it. An encoding of the geography can
// supply the index, so that it is not rebuilt; otherwise it is built when first used.
class IndexedGeography {
 public:
  explicit IndexedGeography(Geography geography);
  IndexedGeography(std::shared_ptr<const Geography> geography,
                   std::shared_ptr<const S2ShapeIndex> index);

  [[nodiscard]] const Geography& geography() const { return *geography_; }
  [[nodiscard]] const S2ShapeIndex& index() const;

 private:
  std::shared_ptr<const Geography> geography_;
  mutable std::shared_ptr<const S2ShapeIndex> index_;
};

// The S2Polygon of the polygons of `geography`.
std::unique_ptr<S2Polygon> ToS2Polygon(const Geography& geography);

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
// merges with itself, and crossing edges are split where they cross, which may leave more than
// one linestring. A long edge gains its midpoint, and a line that snaps to one point is the point
// S2Builder snaps it to.
absl::StatusOr<Geography> BuildLine(const std::vector<S2Point>& vertices);

// The polygon of `rings`, a shell and then its holes, as BigQuery builds one where its
// orientation is not given: a vertex that repeats the one before it is dropped, an edge longer
// than 135 degrees gains its midpoint as a linestring's does, and each ring encloses the smaller of
// the regions it divides the sphere into, which a hole removes from the shell's. Rings must have
// three or more distinct vertices and not cross, and holes must lie inside the shell and outside
// each other; `failed` prefixes the error. Once each ring is valid, vertices within a micrometer
// of each other or an edge snap together, splitting edges at shared vertices. Nondegenerate
// rings may share vertices, but not edges.
// A hole that collapses is dropped, and a shell that collapses is the linestring or the point
// it leaves. An empty first ring denotes a full shell; its holes must be disjoint and
// unnested. Other empty rings are invalid. Where `oriented`, each ring instead encloses the region
// on its left, however large, as the region a hole removes.
absl::StatusOr<Geography> BuildPolygon(const std::vector<std::vector<S2Point>>& input,
                                       std::string_view failed, bool oriented = false);

// Repairs possibly self-intersecting polygon rings with the odd winding rule. The rings need
// not repeat their first vertex. Degenerate input edges are discarded.
absl::StatusOr<Geography> RepairPolygon(const std::vector<std::vector<S2Point>>& input,
                                        std::string_view failed);

// The geography of `parts`, each as BuildLine or BuildPolygon builds it, as BigQuery makes a
// collection: the union of the parts in order, snapped together within a micrometer, where a
// polygon absorbs what it covers, a linestring the points on it, and lines and polygon edges
// that cross gain the vertex where they do. Points follow S2CellId order, and linestrings and
// polygons the order of the parts they came from.
absl::StatusOr<Geography> Combine(const std::vector<Geography>& parts);

// `geography` rebuilt by S2Builder with `snap_function`, where `simplify` also replaces chains of
// edges whose removed vertices stay within the snap radius with one edge. Degenerate polygon
// shells become lines or points, and degenerate lines become points.
absl::StatusOr<Geography> Snap(const Geography& geography,
                               const S2Builder::SnapFunction& snap_function, bool simplify);

// The polygon S2BufferOperation makes of `geography` with `options`: the points within the radius
// of it, or with a negative radius its polygons without the points within the radius of their
// complement. Points and linestrings make nothing where the radius is zero or negative.
absl::StatusOr<Geography> BufferGeography(const Geography& geography,
                                          const S2BufferOperation::Options& options);

// A closed point set operation on two shape indexes already snapped together. Degenerate
// polygon shells become lines or points, and degenerate lines become points.
absl::StatusOr<Geography> ClosedSetOperation(const MutableS2ShapeIndex& a,
                                             const MutableS2ShapeIndex& b,
                                             S2BooleanOperation::OpType operation);

}  // namespace googlesql_geography
