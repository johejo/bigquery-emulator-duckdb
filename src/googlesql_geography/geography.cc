#include "src/googlesql_geography/geography.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <iterator>
#include <limits>
#include <memory>
#include <numbers>
#include <numeric>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "googlesql/base/status_macros.h"
#include "s2/mutable_s2shape_index.h"
#include "s2/r1interval.h"
#include "s2/r2.h"
#include "s2/s1angle.h"
#include "s2/s1chord_angle.h"
#include "s2/s1interval.h"
#include "s2/s2boolean_operation.h"
#include "s2/s2builder.h"
#include "s2/s2builderutil_lax_polygon_layer.h"
#include "s2/s2builderutil_s2point_vector_layer.h"
#include "s2/s2builderutil_s2polyline_vector_layer.h"
#include "s2/s2builderutil_snap_functions.h"
#include "s2/s2cell_id.h"
#include "s2/s2closest_edge_query.h"
#include "s2/s2convex_hull_query.h"
#include "s2/s2debug.h"
#include "s2/s2earth.h"
#include "s2/s2edge_crossings.h"
#include "s2/s2edge_distances.h"
#include "s2/s2edge_tessellator.h"
#include "s2/s2error.h"
#include "s2/s2furthest_edge_query.h"
#include "s2/s2hausdorff_distance_query.h"
#include "s2/s2latlng.h"
#include "s2/s2latlng_rect.h"
#include "s2/s2lax_polygon_shape.h"
#include "s2/s2lax_polyline_shape.h"
#include "s2/s2loop.h"
#include "s2/s2point.h"
#include "s2/s2point_vector_shape.h"
#include "s2/s2polygon.h"
#include "s2/s2polyline.h"
#include "s2/s2projections.h"
#include "s2/s2shape.h"
#include "s2/s2shape_index.h"
#include "s2/s2shape_measures.h"
#include "src/googlesql_geography/options.h"
#include "src/googlesql_geography/value.h"

namespace googlesql_geography {

// The point of a single-point geography, or else an error that `context` starts and that says
// what the geography is, unless `say` is false.
namespace {
absl::StatusOr<S2Point> ParsePoint(const Geography& geography, std::string_view context,
                                   bool say = true) {
  if (geography.Count() != 1 || geography.points.size() != 1) {
    return absl::OutOfRangeError(
        std::string(context) + " must be a single point geography" +
        (!say                          ? "."
         : geography.Empty()           ? "; an argument was the empty geography."
         : !geography.polygons.empty() ? "; an argument contained a polygon."
         : !geography.lines.empty()
             ? "; an argument contained a line."
             : "; an argument had " + Points(geography.points.size()) + "."));
  }
  return geography.points.front();
}
absl::Status CheckFinite(double value, std::string_view name) {
  if (std::isnan(value)) {
    return absl::OutOfRangeError("ST_GeogPoint failed: Value of " + std::string(name) +
                                 " cannot be NaN");
  }
  if (std::isinf(value)) {
    return absl::OutOfRangeError("ST_GeogPoint failed: Value of " + std::string(name) +
                                 " cannot be +-inf");
  }
  return absl::OkStatus();
}
}  // namespace

// The longitude is taken modulo 360 into [-180, 180], 180 staying 180 and 540 becoming -180, as
// std::remainder does; it is exact, where converting to radians, as S2LatLng::Normalized does,
// would round.
absl::StatusOr<Geography> GeogPoint(double longitude, double latitude) {
  GOOGLESQL_RETURN_IF_ERROR(CheckFinite(longitude, "longitude"));
  GOOGLESQL_RETURN_IF_ERROR(CheckFinite(latitude, "latitude"));
  if (std::abs(latitude) > 90) {
    return absl::OutOfRangeError(
        "ST_GeogPoint failed: Latitude must be between -90 and 90 degrees. Actual value was " +
        Coordinate(latitude) + "; check if longitude and latitude arguments order is correct");
  }
  return PointGeography(
      ToS2Point({.longitude = std::remainder(longitude, 360.0), .latitude = latitude}));
}

// The initial bearing over the sphere. Working with longitude differences in degrees
// preserves BigQuery's behavior near antipodes and at poles.
namespace {
double Bearing(const S2Point& p, const S2Point& q) {
  const Point a = ToPoint(p);
  const Point b = ToPoint(q);
  const double lat1 = S1Angle::Degrees(a.latitude).radians();
  const double lat2 = S1Angle::Degrees(b.latitude).radians();
  const double lng = S1Angle::Degrees(b.longitude - a.longitude).radians();
  const double azimuth = std::atan2(
      std::sin(lng) * std::cos(lat2),
      (std::cos(lat1) * std::sin(lat2)) - (std::sin(lat1) * std::cos(lat2) * std::cos(lng)));
  // atan2 is in (-pi, pi]; BigQuery's azimuth is in [0, 2 pi).
  return azimuth < 0 ? azimuth + (2 * std::numbers::pi) : azimuth + 0.0;
}
}  // namespace

// NULL for coincident or antipodal points, where the bearing is undefined.
absl::StatusOr<std::optional<double>> Azimuth(const Geography& a, const Geography& b) {
  GOOGLESQL_ASSIGN_OR_RETURN(const S2Point p, ParsePoint(a, "ST_Azimuth: Argument to STAzimuth"));
  GOOGLESQL_ASSIGN_OR_RETURN(const S2Point q, ParsePoint(b, "ST_Azimuth: Argument to STAzimuth"));
  if (Same(p, q) || Antipodal(p, q)) {
    return std::nullopt;
  }
  return Bearing(p, q);
}

// The clockwise turn at the second point, in [0, 2 pi). Validate all three
// geographies before testing whether either of the two bearings is undefined.
absl::StatusOr<std::optional<double>> Angle(const Geography& a, const Geography& b,
                                            const Geography& c) {
  GOOGLESQL_ASSIGN_OR_RETURN(const S2Point p, ParsePoint(a, "ST_Angle: Argument to STAngle"));
  GOOGLESQL_ASSIGN_OR_RETURN(const S2Point q, ParsePoint(b, "ST_Angle: Argument to STAngle"));
  GOOGLESQL_ASSIGN_OR_RETURN(const S2Point r, ParsePoint(c, "ST_Angle: Argument to STAngle"));
  if (Same(q, p) || Same(q, r) || Antipodal(q, p) || Antipodal(q, r)) {
    return std::nullopt;
  }
  const double angle = Bearing(q, r) - Bearing(q, p);
  return angle < 0 ? angle + (2 * std::numbers::pi) : angle + 0.0;
}

// A distance over the sphere of S2Earth's radius, measured through the chord, which BigQuery's
// distances match to the last digit.
namespace {
double Meters(S1ChordAngle distance) { return S2Earth::ToMeters(distance.ToAngle()); }
}  // namespace

// The shortest distance between nonempty geographies, which is 0 where they meet.
namespace {
S1ChordAngle MinDistance(const IndexedGeography& a, const IndexedGeography& b) {
  S2ClosestEdgeQuery query(&a.index());
  S2ClosestEdgeQuery::ShapeIndexTarget target(&b.index());
  return query.GetDistance(&target);
}
}  // namespace

// The longest distance between nonempty geographies, which BigQuery measures over the edges,
// though ST_MAXDISTANCE describes vertices.
namespace {
S1ChordAngle MaxDistance(const IndexedGeography& a, const IndexedGeography& b) {
  S2FurthestEdgeQuery query(&a.index());
  S2FurthestEdgeQuery::ShapeIndexTarget target(&b.index());
  return query.GetDistance(&target);
}
}  // namespace

// BigQuery's error for use_spheroid TRUE where it is not implemented, which `function`, the
// last argument, names. ST_DISTANCE implements it, and the emulator does not.
namespace {
absl::Status CheckSphere(bool use_spheroid, const std::string& function) {
  if (!use_spheroid) {
    return absl::OkStatus();
  }
  if (function == "ST_DISTANCE") {
    return absl::UnimplementedError("The emulator does not support use_spheroid");
  }
  return absl::OutOfRangeError(
      (function == "ST_CLOSESTPOINT" ? std::string("ST_ClosestPoint failed: ") : std::string()) +
      function + " does not yet implement use_spheroid=true");
}
}  // namespace

// ST_DISTANCE and ST_MAXDISTANCE, which the last argument names.
absl::StatusOr<std::optional<double>> Distance(const IndexedGeography& a, const IndexedGeography& b,
                                               bool use_spheroid, const std::string& function) {
  // Unlike ST_DWITHIN, both distance functions return NULL before checking use_spheroid.
  if (a.geography().Empty() || b.geography().Empty()) {
    return std::nullopt;
  }
  GOOGLESQL_RETURN_IF_ERROR(CheckSphere(use_spheroid, function));
  return Meters(function == "ST_MAXDISTANCE" ? MaxDistance(a, b) : MinDistance(a, b));
}

// Find the closest indexed edge, then the closest edge on the other geography to it.
// Interior results carry a witness point supplied by S2's target visitor.
absl::StatusOr<std::optional<Geography>> ClosestPoint(const IndexedGeography& a,
                                                      const IndexedGeography& b, bool use_spheroid,
                                                      const std::string& function) {
  if (a.geography().Empty() || b.geography().Empty()) {
    return std::nullopt;
  }
  GOOGLESQL_RETURN_IF_ERROR(CheckSphere(use_spheroid, function));
  const S2ShapeIndex& index = a.index();
  const S2ShapeIndex& other = b.index();
  S2ClosestEdgeQuery query(&index);
  S2ClosestEdgeQuery::ShapeIndexTarget target(&other);
  const auto result = query.FindClosestEdge(&target);
  if (result.is_interior()) {
    S2Point point;
    target.VisitContainingShapeIds(index, [&](int, const S2Point& witness) {
      point = witness;
      return false;
    });
    return PointGeography(point);
  }
  const auto edge = query.GetEdge(result);
  S2ClosestEdgeQuery reverse(&other);
  S2ClosestEdgeQuery::EdgeTarget edge_target(edge.v0, edge.v1);
  const auto opposite = reverse.FindClosestEdge(&edge_target);
  if (opposite.is_interior()) {
    return PointGeography((edge.v0 + edge.v1).Normalize());
  }
  const auto other_edge = reverse.GetEdge(opposite);
  return PointGeography(
      S2::GetEdgePairClosestPoints(edge.v0, edge.v1, other_edge.v0, other_edge.v1).first);
}

// A latitude/longitude rectangle has small-circle latitude edges, unlike a WKT polygon.
absl::StatusOr<bool> IntersectsBox(const Geography& geography, double west, double south,
                                   double east, double north) {
  const auto longitude = [](double value) {
    return std::isfinite(value) && std::abs(value) <= 180;
  };
  const bool valid_longitudes =
      (longitude(west) && longitude(east)) ||
      ((longitude(west) || longitude(east)) && east - west >= 0 && east - west <= 360);
  const auto invalid = [&](std::string_view reason) {
    return absl::OutOfRangeError(
        "ST_IntersectsBox: Invalid bounding box (lng1:" + Coordinate(west) +
        ", lat1:" + Coordinate(south) + ", lng2:" + Coordinate(east) +
        ", lat2:" + Coordinate(north) + "): " + std::string(reason));
  };
  if (!std::isfinite(west) || !std::isfinite(east) || !std::isfinite(south) ||
      !std::isfinite(north) || std::abs(south) > 90 || std::abs(north) > 90 ||
      (!longitude(west) && !longitude(east))) {
    return invalid("coordinate out of range.");
  }
  if (!valid_longitudes) {
    return invalid("the longitude span should be less than or equal to 360 degrees.");
  }
  const S1Interval lng = east - west == 360
                             ? S1Interval::Full()
                             : S1Interval(S1Angle::Degrees(std::remainder(west, 360.0)).radians(),
                                          S1Angle::Degrees(std::remainder(east, 360.0)).radians());
  // BigQuery treats [180, -180] specially: it excludes points on the meridian but
  // intersects crossing edges and polygons containing it. Use a valid thin rectangle for
  // its corners, and test only the longitude edges in this case.
  const S2LatLngRect box(R1Interval(S1Angle::Degrees(std::min(south, north)).radians(),
                                    S1Angle::Degrees(std::max(south, north)).radians()),
                         lng.is_empty() ? S1Interval::FromPoint(lng.lo()) : lng);
  const auto boundary_intersects = [&](const S2Shape::Edge& edge) {
    if (!lng.is_empty()) {
      return box.BoundaryIntersects(edge.v0, edge.v1);
    }
    return std::ranges::any_of(std::array{lng.lo(), lng.hi()}, [&](double longitude) {
      return S2::CrossingSign(edge.v0, edge.v1,
                              S2LatLng::FromRadians(box.lat().lo(), longitude).ToPoint(),
                              S2LatLng::FromRadians(box.lat().hi(), longitude).ToPoint()) > 0;
    });
  };
  MutableS2ShapeIndex index;
  AddShapes(geography, index);
  for (const auto& shape : index) {
    for (int i = 0; i < shape->num_edges(); ++i) {
      const auto edge = shape->edge(i);
      if ((!lng.is_empty() && (box.Contains(edge.v0) || box.Contains(edge.v1))) ||
          boundary_intersects(edge)) {
        return true;
      }
    }
  }
  // With no boundary crossing, an intersection can only be a polygon containing the box.
  return ToS2Polygon(geography)->Contains(box.GetVertex(0).ToPoint());
}

// Between single points, the distance itself. Anything else is within the distance where it may be,
// allowing for the error in measuring it: BigQuery takes a point on an edge, a few nanometers
// away as S2 measures it, as within 0 meters, and points that far apart as not.
absl::StatusOr<bool> DWithin(const IndexedGeography& a, const IndexedGeography& b, double limit,
                             bool use_spheroid, const std::string& function) {
  GOOGLESQL_RETURN_IF_ERROR(CheckSphere(use_spheroid, function));
  if (a.geography().Empty() || b.geography().Empty()) {
    return false;
  }
  const auto point = [](const Geography& geography) {
    return geography.Count() == 1 && geography.points.size() == 1;
  };
  if (point(a.geography()) && point(b.geography())) {
    return Meters(MinDistance(a, b)) <= limit;
  }
  if (!(limit >= 0)) {
    return false;
  }
  S2ClosestEdgeQuery query(&a.index());
  S2ClosestEdgeQuery::ShapeIndexTarget target(&b.index());
  return query.IsConservativeDistanceLessOrEqual(&target,
                                                 S1ChordAngle(S2Earth::MetersToAngle(limit)));
}

// The discrete Hausdorff distance measures vertices of the first geography to the second's
// edges and interiors; the undirected form takes the maximum in both directions.
namespace {
S1ChordAngle DiscreteHausdorffDistance(const IndexedGeography& a, const IndexedGeography& b,
                                       bool directed) {
  const S2HausdorffDistanceQuery query;
  return directed ? query.GetDirectedDistance(&a.index(), &b.index())
                  : query.GetDistance(&a.index(), &b.index());
}
}  // namespace

absl::StatusOr<std::optional<double>> HausdorffDistance(const IndexedGeography& a,
                                                        const IndexedGeography& b, bool directed) {
  const S1ChordAngle distance = DiscreteHausdorffDistance(a, b, directed);
  // S2 returns infinity when no distance exists. BigQuery returns NULL, including where a
  // full polygon has no vertices in a direction being measured.
  if (distance.is_infinity()) {
    return std::nullopt;
  }
  return Meters(distance);
}

// The first three arguments are those of HausdorffDistance; the last is the distance limit.
absl::StatusOr<std::optional<bool>> HausdorffDWithin(const IndexedGeography& a,
                                                     const IndexedGeography& b, bool directed,
                                                     double limit) {
  if (std::isnan(limit)) {
    return absl::OutOfRangeError(
        "ST_HausdorffDWithin: ST_HAUSDORFFDWITHIN argument distance cannot be NaN; error in "
        "ST_HAUSDORFFDWITHIN expression");
  }
  if (a.geography().Empty() || b.geography().Empty()) {
    return std::nullopt;
  }
  const S1ChordAngle distance = DiscreteHausdorffDistance(a, b, directed);
  // Unlike ST_HAUSDORFFDISTANCE, the predicate returns FALSE when a full polygon has no
  // vertices in a direction being measured.
  return !distance.is_infinity() && Meters(distance) <= limit;
}

// Whether each point of either of `a` and `b`, nonempty, is within a micrometer of the other, as
// Same takes points.
namespace {
bool Near(const Geography& a, const Geography& b) {
  MutableS2ShapeIndex index;
  AddShapes(a, index);
  MutableS2ShapeIndex other;
  AddShapes(b, other);
  return S2HausdorffDistanceQuery().GetDistance(&index, &other) <=
         S1ChordAngle(S2Earth::MetersToAngle(1e-6));
}
}  // namespace

// Geographies are equal where their points, their linestrings and their polygons are: points
// and linestrings where each point of either is within a micrometer of the other, and polygons
// where they cover the same region but for a micrometer. A point is not a linestring, which has
// more than one point, and a linestring is not a polygon, which has an interior.
absl::StatusOr<bool> Equals(const Geography& a, const Geography& b) {
  if (a.points.empty() != b.points.empty() || a.lines.empty() != b.lines.empty() ||
      a.polygons.empty() != b.polygons.empty()) {
    return false;
  }
  if (!a.points.empty() && !Near({.points = a.points, .lines = {}, .polygons = {}},
                                 {.points = b.points, .lines = {}, .polygons = {}})) {
    return false;
  }
  if (!a.lines.empty() && !Near({.points = {}, .lines = a.lines, .polygons = {}},
                                {.points = {}, .lines = b.lines, .polygons = {}})) {
    return false;
  }
  return a.polygons.empty() ||
         ToS2Polygon(a)->ApproxEquals(*ToS2Polygon(b), S2Earth::MetersToAngle(1e-6));
}

// A geography as S2Builder outputs it, in a layer for its points, each linestring and its
// polygons.
namespace {
struct Snapped {
  std::vector<S2Point> points;
  std::vector<std::unique_ptr<S2Polyline>> polylines;
  std::unique_ptr<S2LaxPolygonShape> polygon = std::make_unique<S2LaxPolygonShape>();
};
}  // namespace

// Adds `geography` to `builder` in layers of its own that output to `output`.
namespace {
void AddLayers(S2Builder& builder, const Geography& geography, Snapped& output) {
  if (!geography.points.empty()) {
    builder.StartLayer(std::make_unique<s2builderutil::S2PointVectorLayer>(&output.points));
    for (const S2Point& point : geography.points) {
      builder.AddPoint(point);
    }
  }
  for (const std::vector<S2Point>& line : geography.lines) {
    // One walk, which gains no endpoints where a vertex is added.
    s2builderutil::S2PolylineVectorLayer::Options options;
    options.set_polyline_type(S2Builder::Graph::PolylineType::WALK);
    builder.StartLayer(
        std::make_unique<s2builderutil::S2PolylineVectorLayer>(&output.polylines, options));
    builder.AddPolyline(line);
  }
  if (!geography.polygons.empty()) {
    builder.StartLayer(std::make_unique<s2builderutil::LaxPolygonLayer>(output.polygon.get()));
    builder.AddPolygon(*ToS2Polygon(geography));
    builder.AddIsFullPolygonPredicate(S2Builder::IsFullPolygon(geography.Full()));
  }
}
}  // namespace

// Adds the shapes of `snapped` to `index`.
namespace {
void AddSnapped(Snapped snapped, MutableS2ShapeIndex& index) {
  if (!snapped.points.empty()) {
    index.Add(std::make_unique<S2PointVectorShape>(std::move(snapped.points)));
  }
  for (std::unique_ptr<S2Polyline>& polyline : snapped.polylines) {
    index.Add(std::make_unique<S2Polyline::OwningShape>(std::move(polyline)));
  }
  if (snapped.polygon->num_loops() > 0) {
    index.Add(std::move(snapped.polygon));
  }
}
}  // namespace

// Applies `predicate` to `a` and `b` snapped together, so that a vertex of one within a
// micrometer of a vertex or an edge of the other meets it there.
namespace {
template <typename Result = bool, typename Predicate>
absl::StatusOr<Result> Relate(const Geography& a, const Geography& b, Predicate predicate) {
  S2Builder builder{
      S2Builder::Options(s2builderutil::IdentitySnapFunction(S2Earth::MetersToAngle(1e-6)))};
  Snapped snapped_a;
  Snapped snapped_b;
  AddLayers(builder, a, snapped_a);
  AddLayers(builder, b, snapped_b);
  S2Error error;
  if (!builder.Build(&error)) {
    return absl::InternalError("S2Builder failed: " + std::string(error.message()));
  }
  MutableS2ShapeIndex index_a;
  AddSnapped(std::move(snapped_a), index_a);
  MutableS2ShapeIndex index_b;
  AddSnapped(std::move(snapped_b), index_b);
  return predicate(index_a, index_b);
}

}  // namespace

// S2's hull can be a thin loop for a line. Normalize its snapped closed set
// so these loops collapse to lines rather than polygons.
absl::StatusOr<std::optional<Geography>> ConvexHull(const Geography& geography) {
  if (geography.Empty()) {
    return std::nullopt;
  }
  // BigQuery preserves a single point; S2's artificial tiny loop can move it when snapped.
  if (geography.Count() == 1 && geography.points.size() == 1) {
    return geography;
  }
  S2ConvexHullQuery query;
  for (const S2Point& point : geography.points) {
    query.AddPoint(point);
  }
  for (const std::vector<S2Point>& line : geography.lines) {
    query.AddPolyline(S2Polyline(line));
  }
  if (!geography.polygons.empty()) {
    query.AddPolygon(*ToS2Polygon(geography));
  }
  const std::unique_ptr<S2Loop> loop = query.GetConvexHull();
  const auto vertices = loop->vertices_span();
  const Geography hull{
      .points = {},
      .lines = {},
      .polygons = {{{vertices.begin(), vertices.end()}}},
  };
  GOOGLESQL_ASSIGN_OR_RETURN(
      Geography result,
      Relate<Geography>(hull, {}, [](const MutableS2ShapeIndex& a, const MutableS2ShapeIndex& b) {
        return ClosedSetOperation(a, b, S2BooleanOperation::OpType::UNION);
      }));
  return result;
}

// Whether `a` and `b` share a point or, with `interiors`, a point of their interiors as OGC
// defines them: a polygon's without its rings, a linestring's without its endpoints unless it
// ends where it starts, and a point's, which is the point.
namespace {
bool Meet(const MutableS2ShapeIndex& a, const MutableS2ShapeIndex& b, bool interiors) {
  using Operation = S2BooleanOperation;
  Operation::Options options;
  options.set_polygon_model(interiors ? Operation::PolygonModel::OPEN
                                      : Operation::PolygonModel::CLOSED);
  options.set_polyline_model(interiors ? Operation::PolylineModel::OPEN
                                       : Operation::PolylineModel::CLOSED);
  options.set_polyline_loops_have_boundaries(false);
  return Operation::Intersects(a, b, options);
}
}  // namespace

// Whether no point of `b` lies outside `a`, whose rings and endpoints it covers.
namespace {
bool Cover(const MutableS2ShapeIndex& a, const MutableS2ShapeIndex& b) {
  using Operation = S2BooleanOperation;
  Operation::Options options;
  options.set_polygon_model(Operation::PolygonModel::CLOSED);
  options.set_polyline_model(Operation::PolylineModel::CLOSED);
  return Operation::Contains(a, b, options);
}
}  // namespace

absl::StatusOr<bool> Intersects(const Geography& a, const Geography& b) {
  if (a.Empty() || b.Empty()) {
    return false;
  }
  return Relate(a, b, [](const MutableS2ShapeIndex& x, const MutableS2ShapeIndex& y) {
    return Meet(x, y, false);
  });
}

// The geographies meet only where the boundary of one is.
absl::StatusOr<bool> Touches(const Geography& a, const Geography& b) {
  if (a.Empty() || b.Empty()) {
    return false;
  }
  return Relate(a, b, [](const MutableS2ShapeIndex& x, const MutableS2ShapeIndex& y) {
    return Meet(x, y, false) && !Meet(x, y, true);
  });
}

absl::StatusOr<bool> Covers(const Geography& a, const Geography& b) {
  if (a.Empty() || b.Empty()) {
    return false;
  }
  return Relate(a, b, Cover);
}

// `a` covers `b`, and their interiors meet, so a geography does not contain its boundary.
absl::StatusOr<bool> Contains(const Geography& a, const Geography& b) {
  if (a.Empty() || b.Empty()) {
    return false;
  }
  return Relate(a, b, [](const MutableS2ShapeIndex& x, const MutableS2ShapeIndex& y) {
    return Cover(x, y) && Meet(x, y, true);
  });
}

absl::StatusOr<double> X(const Geography& geography) {
  GOOGLESQL_ASSIGN_OR_RETURN(const S2Point point, ParsePoint(geography, "Argument to ST_X"));
  return ToPoint(point).longitude;
}

absl::StatusOr<double> Y(const Geography& geography) {
  GOOGLESQL_ASSIGN_OR_RETURN(const S2Point point, ParsePoint(geography, "Argument to ST_Y"));
  return ToPoint(point).latitude;
}

// A closed point set operation after snapping both inputs together.
absl::StatusOr<Geography> SetOperation(const Geography& a, const Geography& b,
                                       S2BooleanOperation::OpType operation) {
  GOOGLESQL_ASSIGN_OR_RETURN(
      Geography result,
      Relate<Geography>(a, b,
                        [operation](const MutableS2ShapeIndex& x, const MutableS2ShapeIndex& y) {
                          return ClosedSetOperation(x, y, operation);
                        }));
  return result;
}

absl::StatusOr<Geography> Boundary(const Geography& geography) {
  std::vector<Geography> parts;
  for (const std::vector<S2Point>& line : geography.lines) {
    if (!Same(line.front(), line.back())) {
      parts.push_back(PointGeography(line.front()));
      parts.push_back(PointGeography(line.back()));
    }
  }
  if (!geography.Full()) {
    for (const Polygon& polygon : geography.polygons) {
      for (const std::vector<S2Point>& loop : polygon) {
        std::vector<S2Point> line = loop;
        line.push_back(loop.front());
        parts.push_back({.points = {}, .lines = {std::move(line)}, .polygons = {}});
      }
    }
  }
  GOOGLESQL_ASSIGN_OR_RETURN(Geography boundary, Combine(parts));
  return boundary;
}

namespace {

// The bounds of all the parts of `geographies` together, which no empty geography changes. With
// `extent`, as ST_EXTENT has them, longitudes that the smallest interval covers over 180 or more
// degrees become all of them.
std::optional<BoundingBox> BoundsOf(std::span<const Geography> geographies, const Options& options,
                                    bool extent) {
  if (std::ranges::all_of(geographies, &Geography::Empty)) {
    return std::nullopt;
  }
  BoundingBox box{.xmin = -180, .ymin = 90, .xmax = 180, .ymax = -90};
  std::vector<std::pair<double, double>> longitudes;
  const auto add = [&](const S2LatLngRect& bound) {
    box.ymin = std::min(box.ymin, bound.lat_lo().degrees());
    box.ymax = std::max(box.ymax, bound.lat_hi().degrees());
    const double lo = bound.lng_lo().degrees();
    const double hi = bound.lng_hi().degrees();
    if (bound.lng().is_inverted()) {
      longitudes.emplace_back(-180, hi);
      longitudes.emplace_back(lo, 180);
    } else {
      longitudes.emplace_back(lo, hi);
    }
  };
  for (const Geography& geography : geographies) {
    for (const S2Point& point : geography.points) {
      add(S2LatLngRect::FromPoint(S2LatLng(point)));
    }
    for (const std::vector<S2Point>& line : geography.lines) {
      add(S2Polyline(line).GetRectBound());
    }
    if (!geography.polygons.empty()) {
      add(ToS2Polygon(geography)->GetRectBound());
    }
  }
  // The complement of the largest uncovered longitude gap is the smallest interval. Taking
  // pairwise rectangle unions can miss it when there are several disconnected components.
  std::ranges::sort(longitudes);
  double end = longitudes.front().second;
  const double last = std::ranges::max(longitudes, {}, &std::pair<double, double>::second).second;
  double gap = longitudes.front().first + 360 - last;
  box.xmin = longitudes.front().first;
  box.xmax = last;
  for (size_t i = 1; i < longitudes.size(); ++i) {
    const auto [lo, hi] = longitudes.at(i);
    if (lo - end > gap) {
      gap = lo - end;
      box.xmin = lo;
      box.xmax = end + 360;
    }
    end = std::max(end, hi);
  }
  if (extent && box.xmax - box.xmin >= 180) {
    box.xmin = -180;
    box.xmax = 180;
    gap = 0;
  }
  // BigQuery rounds each bound outward by one representable degree value, including zero,
  // whose neighbors are subnormal. Full longitude intervals and the poles stay in range.
  if (!options.expand_bounding_box) {
    return box;
  }
  const double infinity = std::numeric_limits<double>::infinity();
  if (gap > 0) {
    box.xmin = std::nextafter(box.xmin, -infinity);
    box.xmax = std::nextafter(box.xmax, infinity);
  }
  box.ymin = std::max(-90.0, std::nextafter(box.ymin, -infinity));
  box.ymax = std::min(90.0, std::nextafter(box.ymax, infinity));
  return box;
}

}  // namespace

std::optional<BoundingBox> Bounds(const Geography& geography, const Options& options) {
  return BoundsOf(std::span(&geography, 1), options, /*extent=*/false);
}

std::optional<BoundingBox> Extent(std::span<const Geography> geographies, const Options& options) {
  return BoundsOf(geographies, options, /*extent=*/true);
}

// The highest dimension of the parts of a geography, -1 for the empty geography.
int64_t Dimension(const Geography& geography) {
  return !geography.polygons.empty() ? 2
         : !geography.lines.empty()  ? 1
         : !geography.points.empty() ? 0
                                     : -1;
}

// The geometry type of the WKT that Wkt writes, which BigQuery spells as ST_GEOMETRYTYPE returns
// it.
std::string GeometryType(const Geography& geography) {
  const size_t dimensions = (geography.points.empty() ? 0 : 1) + (geography.lines.empty() ? 0 : 1) +
                            (geography.polygons.empty() ? 0 : 1);
  if (dimensions != 1) {
    return "ST_GeometryCollection";
  }
  const auto type = [&](size_t count, std::string_view name) {
    return std::string(count == 1 ? "ST_" : "ST_Multi") + std::string(name);
  };
  if (!geography.points.empty()) {
    return type(geography.points.size(), "Point");
  }
  if (!geography.lines.empty()) {
    return type(geography.lines.size(), "LineString");
  }
  return type(geography.polygons.size(), "Polygon");
}

// A polygon's rings count their first vertex twice, as WKT writes them.
int64_t NumPoints(const Geography& geography) {
  if (geography.Full()) {
    return 0;
  }
  const auto lines = std::accumulate(
      geography.lines.begin(), geography.lines.end(), geography.points.size(),
      [](size_t count, const std::vector<S2Point>& line) { return count + line.size(); });
  return static_cast<int64_t>(std::accumulate(
      geography.polygons.begin(), geography.polygons.end(), lines,
      [](size_t count, const Polygon& polygon) {
        return std::accumulate(
            polygon.begin(), polygon.end(), count,
            [](size_t sum, const std::vector<S2Point>& loop) { return sum + loop.size() + 1; });
      }));
}

// A nonempty geography whose parts are all closed: a point is closed, and a linestring where it
// ends where it starts. Only the full polygon is closed.
bool IsClosed(const Geography& geography) {
  if (geography.Full()) {
    return true;
  }
  return !geography.Empty() && geography.polygons.empty() &&
         std::ranges::all_of(geography.lines, [](const std::vector<S2Point>& line) {
           return Same(line.front(), line.back());
         });
}

// A ring is one closed linestring with no repeated point except its endpoints.
// S2Loop validates both repeated vertices and crossings between edges.
bool IsRing(const Geography& geography) {
  if (geography.Count() != 1 || geography.lines.size() != 1 || !IsClosed(geography)) {
    return false;
  }
  const std::vector<S2Point>& line = geography.lines.front();
  if (line.size() < 4) {
    return false;
  }
  const std::vector<S2Point> vertices(line.begin(), line.end() - 1);
  const S2Loop loop(vertices, S2Debug::DISABLE);
  S2Error error;
  return !loop.FindValidationError(&error);
}

namespace {

// The centroid of the parts of `geography` of `dimension`, scaled by their measure, as S2's
// centroids are, so that the sum over several gives their weighted centroid.
S2Point WeightedCentroid(const Geography& geography, int64_t dimension) {
  if (dimension == 2) {
    return geography.polygons.empty() ? S2Point(0, 0, 0) : ToS2Polygon(geography)->GetCentroid();
  }
  if (dimension == 1) {
    return std::accumulate(geography.lines.begin(), geography.lines.end(), S2Point(0, 0, 0),
                           [](const S2Point& sum, const std::vector<S2Point>& line) {
                             return sum + S2::GetCentroid(S2LaxPolylineShape(line));
                           });
  }
  return std::accumulate(geography.points.begin(), geography.points.end(), S2Point(0, 0, 0));
}

// Only the highest dimension of `geographies` contributes, and the empty geography where they have
// none.
Geography CentroidOf(std::span<const Geography> geographies) {
  const int64_t dimension = std::ranges::max(geographies | std::views::transform(Dimension));
  if (dimension < 0) {
    return {};
  }
  const S2Point centroid =
      std::accumulate(geographies.begin(), geographies.end(), S2Point(0, 0, 0),
                      [dimension](const S2Point& sum, const Geography& geography) {
                        return sum + WeightedCentroid(geography, dimension);
                      });
  if (centroid.Norm2() == 0) {
    return PointGeography(S2Point(1, 0, 0));
  }
  return PointGeography(centroid.Normalize());
}

}  // namespace

absl::StatusOr<Geography> Centroid(const Geography& geography) {
  return CentroidOf(std::span(&geography, 1));
}

// ST_CENTROID_AGG, which weights each geography as ST_CENTROID weights the parts of one, so a
// repeated geography counts each time; NULL for no geographies.
std::optional<Geography> CentroidAggregate(const std::vector<Geography>& geographies) {
  if (geographies.empty()) {
    return std::nullopt;
  }
  return CentroidOf(geographies);
}

// ST_LENGTH, the length of the edges of the linestrings, which is 0 for points, polygons and the
// empty geography.
absl::StatusOr<double> Length(const Geography& geography, bool use_spheroid,
                              const std::string& function) {
  GOOGLESQL_RETURN_IF_ERROR(CheckSphere(use_spheroid, function));
  return std::accumulate(geography.lines.begin(), geography.lines.end(), 0.0,
                         [](double length, const std::vector<S2Point>& line) {
                           return length +
                                  S2Earth::ToMeters(S2::GetLength(S2LaxPolylineShape(line)));
                         });
}

// ST_AREA and ST_PERIMETER, which the last argument names, of the polygons over the sphere, and 0
// where there are none.
absl::StatusOr<double> Measure(const Geography& geography, bool use_spheroid,
                               const std::string& function) {
  GOOGLESQL_RETURN_IF_ERROR(CheckSphere(use_spheroid, function));
  if (geography.polygons.empty()) {
    return 0.0;
  }
  const std::unique_ptr<S2Polygon> polygon = ToS2Polygon(geography);
  if (function == "ST_AREA") {
    return S2Earth::SteradiansToSquareMeters(polygon->GetArea());
  }
  return S2Earth::ToMeters(S2::GetPerimeter(S2Polygon::Shape(polygon.get())));
}

namespace {

// "[ " and "]" around `items`, separated by commas, as BigQuery's GeoJSON writes arrays of arrays.
std::string GeoJsonArray(const std::vector<std::string>& items) {
  std::string text = "[ ";
  for (const std::string& item : items) {
    text += (&item == items.data() ? "" : ", ") + item;
  }
  return text + " ]";
}

std::string GeoJsonPosition(double longitude, double latitude) {
  return "[" + Coordinate(longitude) + ", " + Coordinate(latitude) + "]";
}

std::string GeoJsonPoint(const S2Point& point) {
  const Point coordinates = ToPoint(point);
  return GeoJsonPosition(coordinates.longitude, coordinates.latitude);
}

// The positions of a chain of `vertices`, with the vertices S2EdgeTessellator adds between them.
// The error names a chain whose GeoJSON BigQuery writes otherwise: one that touches a pole, whose
// longitude the projection does not give, or that crosses the antimeridian, which the tessellator
// continues beyond longitude 180 where BigQuery splits it.
absl::StatusOr<std::string> GeoJsonChain(const std::vector<S2Point>& vertices) {
  const S2::PlateCarreeProjection projection(180);
  const S2EdgeTessellator tessellator(&projection, S2Earth::MetersToAngle(10));
  std::vector<R2Point> projected;
  for (auto a = vertices.begin(); a != vertices.end() && std::next(a) != vertices.end(); ++a) {
    const S2Point& b = *std::next(a);
    for (const S2Point& pole : {S2Point(0, 0, 1), S2Point(0, 0, -1)}) {
      if (S2::GetDistance(pole, *a, b) < S2EdgeTessellator::kMinTolerance()) {
        return absl::UnimplementedError(" that touches a pole");
      }
    }
    tessellator.AppendProjected(*a, b, &projected);
  }
  std::vector<std::string> positions;
  for (const R2Point& vertex : projected) {
    if (std::abs(vertex.x()) > 180) {
      return absl::UnimplementedError(" that crosses the antimeridian");
    }
    positions.push_back(GeoJsonPosition(vertex.x(), vertex.y()));
  }
  return GeoJsonArray(positions);
}

// The GeoJSON object of `items`, the points, linestrings or polygons of a geography, each as
// `write` writes its coordinates, without BigQuery's trailing space.
template <typename Item, typename Write>
absl::StatusOr<std::string> GeoJsonParts(const std::vector<Item>& items, std::string_view one,
                                         std::string_view many, Write write) {
  std::vector<std::string> coordinates;
  for (const Item& item : items) {
    GOOGLESQL_ASSIGN_OR_RETURN(std::string text, write(item));
    coordinates.push_back(std::move(text));
  }
  return R"({ "type": ")" + std::string(items.size() == 1 ? one : many) + R"(", "coordinates": )" +
         (items.size() == 1 ? coordinates.front() : GeoJsonArray(coordinates)) + " }";
}

}  // namespace

// BigQuery writes the parts of a geography as its WKT does, and adds vertices to keep each planar
// edge within 10 meters of the geodesic one, as S2EdgeTessellator does; their last bits depend on
// the platform's math library, such as one vertex of LINESTRING(0 0, 10 10) with macOS's atan2.
absl::StatusOr<std::string> AsGeoJson(const Geography& geography) {
  if (geography.Full()) {
    return "{ \"type\": \"Polygon\", \"coordinates\": [ [ [180, 90], [-180, 90], [-180, -90], "
           "[180, -90], [180, 90] ] ] } ";
  }
  if (geography.Empty()) {
    return std::string(R"({ "type": "GeometryCollection", "geometries": [ ] } )");
  }
  const auto rings = [](const Polygon& polygon) -> absl::StatusOr<std::string> {
    std::vector<std::string> items;
    for (const std::vector<S2Point>& loop : polygon) {
      std::vector<S2Point> ring = loop;
      if (&loop != polygon.data()) {
        std::ranges::reverse(ring);
      }
      ring.push_back(ring.front());
      GOOGLESQL_ASSIGN_OR_RETURN(std::string text, GeoJsonChain(ring));
      items.push_back(std::move(text));
    }
    return GeoJsonArray(items);
  };
  const auto write = [&]() -> absl::StatusOr<std::vector<std::string>> {
    std::vector<std::string> parts;
    if (!geography.points.empty()) {
      GOOGLESQL_ASSIGN_OR_RETURN(
          std::string part, GeoJsonParts(geography.points, "Point", "MultiPoint",
                                         [](const S2Point& point) -> absl::StatusOr<std::string> {
                                           return GeoJsonPoint(point);
                                         }));
      parts.push_back(std::move(part));
    }
    if (!geography.lines.empty()) {
      GOOGLESQL_ASSIGN_OR_RETURN(std::string part, GeoJsonParts(geography.lines, "LineString",
                                                                "MultiLineString", GeoJsonChain));
      parts.push_back(std::move(part));
    }
    if (!geography.polygons.empty()) {
      GOOGLESQL_ASSIGN_OR_RETURN(
          std::string part, GeoJsonParts(geography.polygons, "Polygon", "MultiPolygon", rings));
      parts.push_back(std::move(part));
    }
    return parts;
  };
  const absl::StatusOr<std::vector<std::string>> parts = write();
  if (!parts.ok()) {
    std::string type = GeometryType(geography).substr(3);
    std::ranges::transform(type, type.begin(),
                           [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return absl::UnimplementedError("The emulator does not support ST_ASGEOJSON of a " + type +
                                    std::string(parts.status().message()));
  }
  if (parts->size() == 1) {
    return parts->front() + " ";
  }
  return R"({ "type": "GeometryCollection", "geometries": )" + GeoJsonArray(*parts) + " } ";
}

// ST_STARTPOINT, ST_ENDPOINT and ST_POINTN: the vertex of a linestring at a 1-based index, a
// negative one counting from the end.
absl::StatusOr<Geography> PointN(const Geography& geography, int64_t index) {
  if (geography.Count() != 1 || geography.lines.empty()) {
    return absl::OutOfRangeError("The input is not a single LINESTRING");
  }
  const std::vector<S2Point>& line = geography.lines.front();
  const auto size = static_cast<int64_t>(line.size());
  if (index == 0 || index > size || index < -size) {
    return absl::OutOfRangeError("The vertex index " + std::to_string(index) +
                                 " is out of bounds; input LINESTRING has " + Points(line.size()) +
                                 ".");
  }
  return PointGeography(line.at(index > 0 ? index - 1 : size + index));
}

// BigQuery names the operation and the kind of invalid component in its errors.
namespace {
absl::Status CheckLine(const Geography& geography, std::string_view function,
                       std::string_view context, bool allow_empty, bool prefix_function = true) {
  if ((allow_empty && geography.Empty()) ||
      (geography.Count() == 1 && geography.lines.size() == 1)) {
    return absl::OkStatus();
  }
  const std::string reason = geography.Empty()             ? "was the empty geography"
                             : !geography.polygons.empty() ? "contained a polygon"
                             : !geography.points.empty()
                                 ? "contained a point"
                                 : "had " + std::to_string(geography.lines.size()) + " lines";
  return absl::OutOfRangeError((prefix_function ? std::string(function) + ": " : "") +
                               "Argument to " + std::string(context) +
                               " must be a single line geography; an argument " + reason +
                               ". Error in " + std::string(function) + " expression");
}
}  // namespace

absl::StatusOr<Geography> LineInterpolatePoint(const Geography& geography, double fraction) {
  if (!std::isfinite(fraction) || fraction < 0 || fraction > 1) {
    return absl::OutOfRangeError(std::format(
        "ST_LINEINTERPOLATEPOINT: ST_LineInterpolatePoint: fraction should be between 0 and 1. "
        "Actual value was {:.6f}; error in ST_LINEINTERPOLATEPOINT expression",
        fraction));
  }
  GOOGLESQL_RETURN_IF_ERROR(
      CheckLine(geography, "ST_LINEINTERPOLATEPOINT", "ST_LineInterpolatePoint", true));
  if (geography.Empty()) {
    return geography;
  }
  return PointGeography(S2Polyline(geography.lines.front()).Interpolate(fraction));
}

// Project onto the geodesic line, then measure the fraction along its edges.
// Arguments follow ST_LINELOCATEPOINT's SQL order.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
absl::StatusOr<double> LineLocatePoint(const Geography& geography,
                                       const Geography& point_geography) {
  GOOGLESQL_RETURN_IF_ERROR(
      CheckLine(geography, "ST_LINELOCATEPOINT", "STLineLocatePoint", false, false));
  const auto point = ParsePoint(point_geography, "Argument to STLineLocatePoint");
  if (!point.ok()) {
    return absl::Status(point.status().code(), std::string(point.status().message()) +
                                                   " Error in ST_LINELOCATEPOINT expression");
  }
  const S2Polyline line(geography.lines.front());
  int next_vertex = 0;
  const S2Point projected = line.Project(*point, &next_vertex);
  return line.UnInterpolate(projected, next_vertex);
}

absl::StatusOr<Geography> LineSubstring(const Geography& geography, double start, double end) {
  GOOGLESQL_RETURN_IF_ERROR(CheckLine(geography, "ST_LINESUBSTRING", "STLineSubString", false));
  if (!std::isfinite(start) || start < 0 || start > 1) {
    return absl::OutOfRangeError(
        "ST_LINESUBSTRING: Second argument to STLineSubString must be a float between 0.0 and 1.0. "
        "Error in ST_LINESUBSTRING expression");
  }
  if (!std::isfinite(end) || end < 0 || end > 1) {
    return absl::OutOfRangeError(
        "ST_LINESUBSTRING: Third argument to STLineSubString must be a float between 0.0 and 1.0. "
        "Error in ST_LINESUBSTRING expression");
  }
  if (start > end) {
    return absl::OutOfRangeError(
        "ST_LINESUBSTRING: Second argument to STLineSubString must be <= Third argument. Error in "
        "ST_LINESUBSTRING expression");
  }
  const S2Polyline line(geography.lines.front());
  int next_start = 0;
  int next_end = 0;
  const S2Point first = line.GetSuffix(start, &next_start);
  if (start == end) {
    return PointGeography(first);
  }
  const S2Point last = line.GetSuffix(end, &next_end);
  std::vector<S2Point> vertices{first};
  for (int i = next_start; i < next_end; ++i) {
    vertices.push_back(line.vertex(i));
  }
  if (vertices.back() != last) {
    vertices.push_back(last);
  }
  GOOGLESQL_ASSIGN_OR_RETURN(Geography result, BuildLine(vertices));
  return result;
}

// ST_UNION, with NULL array elements already removed by the translator.
absl::StatusOr<std::optional<Geography>> UnionGeographies(
    const std::vector<Geography>& geographies) {
  if (geographies.empty()) {
    return std::nullopt;
  }
  GOOGLESQL_ASSIGN_OR_RETURN(Geography geography, Combine(geographies));
  return geography;
}

// The line through the vertices of `geographies`, each a single point or linestring, built as
// ST_GEOGFROMTEXT builds one; a single point is that point.
absl::StatusOr<Geography> MakeLine(const std::vector<Geography>& geographies) {
  std::vector<S2Point> vertices;
  for (const Geography& geography : geographies) {
    if (geography.Count() != 1 || !geography.polygons.empty()) {
      return absl::OutOfRangeError("ST_MAKELINE arguments must be single points or lines");
    }
    const std::vector<S2Point>& part =
        geography.points.empty() ? geography.lines.front() : geography.points;
    vertices.insert(vertices.end(), part.begin(), part.end());
  }
  if (vertices.empty()) {
    return absl::OutOfRangeError(
        "ST_MAKELINE failed: Linestring should have at least two points, but had " +
        Points(vertices.size()));
  }
  if (vertices.size() == 1) {
    return PointGeography(vertices.front());
  }
  std::vector<Point> points(vertices.size());
  std::ranges::transform(vertices, points.begin(), ToPoint);
  GOOGLESQL_RETURN_IF_ERROR(CheckAntipodal(points, ""));
  GOOGLESQL_ASSIGN_OR_RETURN(Geography line, BuildLine(vertices));
  return line;
}

// The polygon whose rings are the linestrings of `geographies`, a shell and then its holes, each
// closed by an edge from its last vertex to its first where it does not end where it starts.
absl::StatusOr<Geography> MakePolygon(const std::vector<Geography>& geographies) {
  std::vector<std::vector<S2Point>> rings;
  for (const Geography& geography : geographies) {
    if (geography.Empty()) {
      rings.emplace_back();
      continue;
    }
    if (geography.Count() != 1 || geography.lines.empty()) {
      return absl::OutOfRangeError(
          "ST_MakePolygon failed: Input geographies for STMakePolygon must be empty or consist of "
          "exactly one linestring.");
    }
    std::vector<S2Point>& ring = rings.emplace_back(geography.lines.front());
    if (ring.front() == ring.back()) {
      ring.pop_back();
    }
  }
  GOOGLESQL_ASSIGN_OR_RETURN(Geography polygon, BuildPolygon(rings, "ST_MakePolygon failed: "));
  return polygon;
}

// A loop of a polygon as a linestring that ends where it starts.
namespace {
Geography Ring(const std::vector<S2Point>& loop) {
  std::vector<S2Point> line = loop;
  line.push_back(loop.front());
  return Geography{.points = {}, .lines = {std::move(line)}, .polygons = {}};
}
}  // namespace

// ST_EXTERIORRING and ST_INTERIORRINGS, which `function` names as BigQuery's errors do: the loops
// of a polygon as S2Polygon holds them, a hole enclosing the region it removes. A collection is
// not a polygon, even of one polygon with points or linestrings.
namespace {
absl::StatusOr<std::vector<Geography>> Rings(const Geography& geography,
                                             std::string_view function) {
  if (geography.polygons.size() != 1 || geography.Count() != 1) {
    return absl::OutOfRangeError(
        "Argument to " + std::string(function) +
        " must have exactly 1 polygon; argument either had no polygons or more than one.");
  }
  const Polygon& polygon = geography.polygons.front();
  if (geography.Full()) {
    return std::vector<Geography>{Geography{}};
  }
  std::vector<Geography> rings(polygon.size());
  std::ranges::transform(polygon, rings.begin(), Ring);
  return rings;
}
}  // namespace

// ST_DUMP: each point, linestring and polygon of a geography, in the order Wkt writes them, of
// the dimension of the second argument, or of any for -1.
absl::StatusOr<std::vector<Geography>> Dump(const Geography& geography, int64_t dimension) {
  if (dimension < -1 || dimension > 2) {
    return absl::OutOfRangeError("Dimension must be -1, 0, 1, or 2, but was " +
                                 std::to_string(dimension) + ". Error in ST_DUMP expression");
  }
  const auto wanted = [&](int64_t of) { return dimension == -1 || dimension == of; };
  std::vector<Geography> parts;
  if (wanted(0)) {
    std::ranges::transform(geography.points, std::back_inserter(parts),
                           [](const S2Point& point) { return PointGeography(point); });
  }
  if (wanted(1)) {
    std::ranges::transform(geography.lines, std::back_inserter(parts),
                           [](const std::vector<S2Point>& line) {
                             return Geography{.points = {}, .lines = {line}, .polygons = {}};
                           });
  }
  if (wanted(2)) {
    std::ranges::transform(geography.polygons, std::back_inserter(parts),
                           [](const Polygon& polygon) {
                             return Geography{.points = {}, .lines = {}, .polygons = {polygon}};
                           });
  }
  return parts;
}

constexpr std::string_view kGeohashDigits = "0123456789bcdefghjkmnpqrstuvwxyz";

// A maxchars outside [1, 20] is taken as 20.
absl::StatusOr<std::optional<std::string>> Geohash(const Geography& geography, int64_t length) {
  if (geography.Empty()) {
    return std::nullopt;
  }
  GOOGLESQL_ASSIGN_OR_RETURN(const S2Point vertex,
                             ParsePoint(geography, "ST_GeoHash: Argument to ST_GeoHash", false));
  const Point point = ToPoint(vertex);
  if (length < 1 || length > 20) {
    length = 20;
  }
  // Longitude bits come first and alternate with latitude bits, five to a digit.
  std::array<std::array<double, 2>, 2> ranges = {{{-180, 180}, {-90, 90}}};
  const std::array<double, 2> coordinates = {point.longitude, point.latitude};
  std::string hash;
  unsigned digit = 0;
  for (size_t i = 0; std::cmp_less(hash.size(), length); ++i) {
    std::array<double, 2>& range = ranges.at(i % 2);
    const double middle = (range.at(0) + range.at(1)) / 2;
    const bool upper = coordinates.at(i % 2) >= middle;
    range.at(upper ? 0 : 1) = middle;
    digit = (digit << 1U) | (upper ? 1U : 0U);
    if (i % 5 == 4) {
      hash += kGeohashDigits.at(digit);
      digit = 0;
    }
  }
  return hash;
}

// The middle of the GeoHash's box.
absl::StatusOr<Geography> PointFromGeohash(const std::string& hash) {
  const auto invalid = [&] {
    return absl::OutOfRangeError("ST_GeogPointFromGeoHash failed: Invalid geohash '" + hash + "'.");
  };
  if (hash.empty()) {
    return invalid();
  }
  std::array<std::array<double, 2>, 2> ranges = {{{-180, 180}, {-90, 90}}};
  size_t i = 0;
  for (const char c : hash) {
    const size_t digit =
        kGeohashDigits.find(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    if (digit == std::string_view::npos) {
      return invalid();
    }
    for (size_t shift = 5; shift-- > 0; ++i) {
      std::array<double, 2>& range = ranges.at(i % 2);
      range.at(((digit >> shift) & 1U) != 0 ? 0 : 1) = (range.at(0) + range.at(1)) / 2;
    }
  }
  const auto middle = [&](size_t axis) {
    return (ranges.at(axis).at(0) + ranges.at(axis).at(1)) / 2;
  };
  return PointGeography(ToS2Point({.longitude = middle(0), .latitude = middle(1)}));
}

absl::StatusOr<int64_t> CellIdFromPoint(const Geography& geography, int64_t level) {
  if (level < 0 || level > S2CellId::kMaxLevel) {
    return absl::OutOfRangeError(
        "S2_CellIdFromPoint: S2CellIdFromPoint failed: level argument must be in range [0,30], "
        "got " +
        std::to_string(level));
  }
  GOOGLESQL_ASSIGN_OR_RETURN(
      const S2Point point,
      ParsePoint(geography, "S2_CellIdFromPoint: Argument to S2CellIdFromPoint"));
  return std::bit_cast<int64_t>(S2CellId(point).parent(static_cast<int>(level)).id());
}

absl::StatusOr<Geography> ExteriorRing(const Geography& geography) {
  GOOGLESQL_ASSIGN_OR_RETURN(const std::vector<Geography> rings,
                             Rings(geography, "STExteriorRing"));
  return rings.front();
}

absl::StatusOr<std::vector<Geography>> InteriorRings(const Geography& geography) {
  auto rings = Rings(geography, "STInteriorRings");
  if (!rings.ok()) {
    return absl::OutOfRangeError(std::string(rings.status().message()) +
                                 " Error in ST_INTERIORRINGS expression");
  }
  rings->erase(rings->begin());
  return *std::move(rings);
}

}  // namespace googlesql_geography
