#include "src/backend_functions/geography.h"

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
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "duckdb.h"
#include "googlesql/base/status_macros.h"
#include "s2/mutable_s2shape_index.h"
#include "s2/s1angle.h"
#include "s2/s1chord_angle.h"
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
#include "s2/s2shape_measures.h"
#include "src/backend_functions/geography_value.h"
#include "src/backend_functions/geography_wkt.h"
#include "src/backend_functions/register.h"
#include "src/backend_functions/scalar.h"
#include "src/duckdb_handle.h"

namespace bigquery_emulator_duckdb::backend_functions {
namespace {

// The point of a single-point geography, or else an error that `context` starts and that says
// what the geography is, unless `say` is false.
absl::StatusOr<S2Point> ParsePoint(const Arguments& args, idx_t index, std::string_view context,
                                   bool say = true) {
  GOOGLESQL_ASSIGN_OR_RETURN(const Geography geography, ParseGeography(args.String(index)));
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

// The longitude is taken modulo 360 into [-180, 180], 180 staying 180 and 540 becoming -180, as
// std::remainder does; it is exact, where converting to radians, as S2LatLng::Normalized does,
// would round.
absl::StatusOr<std::string> GeogPoint(const Arguments& args) {
  const double longitude = args.Double(0);
  const double latitude = args.Double(1);
  GOOGLESQL_RETURN_IF_ERROR(CheckFinite(longitude, "longitude"));
  GOOGLESQL_RETURN_IF_ERROR(CheckFinite(latitude, "latitude"));
  if (std::abs(latitude) > 90) {
    return absl::OutOfRangeError(
        "ST_GeogPoint failed: Latitude must be between -90 and 90 degrees. Actual value was " +
        Coordinate(latitude) + "; check if longitude and latitude arguments order is correct");
  }
  return GeographyText(PointGeography(
      ToS2Point({.longitude = std::remainder(longitude, 360.0), .latitude = latitude})));
}

absl::StatusOr<std::string> GeogFromText(const Arguments& args) {
  GOOGLESQL_ASSIGN_OR_RETURN(const Geography geography, ReadWkt(args.String(0)));
  return GeographyText(geography);
}

// A GEOGRAPHY that a load or tabledata.insertAll writes, given as WKT whose polygons are oriented.
// BigQuery also takes GeoJSON and WKB, hex-encoded or, from Parquet, binary, which DuckDB casts to
// text that starts with the escape of its first byte; these are unsupported.
absl::StatusOr<std::string> GeographyInput(const Arguments& args) {
  const std::string text = args.String(0);
  const size_t start = text.find_first_not_of(" \t\r\n");
  if (start != std::string::npos && text.at(start) == '{') {
    return absl::UnimplementedError("The emulator does not support GeoJSON GEOGRAPHY values");
  }
  if (text.starts_with("\\x") || (!text.empty() && std::ranges::all_of(text, [](unsigned char c) {
        return std::isxdigit(c) != 0;
      }))) {
    return absl::UnimplementedError("The emulator does not support WKB GEOGRAPHY values");
  }
  GOOGLESQL_ASSIGN_OR_RETURN(const Geography geography, ReadWkt(text, /*oriented=*/true));
  return GeographyText(geography);
}

// The initial bearing over the sphere. Working with longitude differences in degrees
// preserves BigQuery's behavior near antipodes and at poles.
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

// NULL for coincident or antipodal points, where the bearing is undefined.
absl::StatusOr<std::optional<double>> Azimuth(const Arguments& args) {
  GOOGLESQL_ASSIGN_OR_RETURN(const S2Point p,
                             ParsePoint(args, 0, "ST_Azimuth: Argument to STAzimuth"));
  GOOGLESQL_ASSIGN_OR_RETURN(const S2Point q,
                             ParsePoint(args, 1, "ST_Azimuth: Argument to STAzimuth"));
  if (Same(p, q) || Antipodal(p, q)) {
    return std::nullopt;
  }
  return Bearing(p, q);
}

// The clockwise turn at the second point, in [0, 2 pi). Validate all three
// geographies before testing whether either of the two bearings is undefined.
absl::StatusOr<std::optional<double>> Angle(const Arguments& args) {
  GOOGLESQL_ASSIGN_OR_RETURN(const S2Point p, ParsePoint(args, 0, "ST_Angle: Argument to STAngle"));
  GOOGLESQL_ASSIGN_OR_RETURN(const S2Point q, ParsePoint(args, 1, "ST_Angle: Argument to STAngle"));
  GOOGLESQL_ASSIGN_OR_RETURN(const S2Point r, ParsePoint(args, 2, "ST_Angle: Argument to STAngle"));
  if (Same(q, p) || Same(q, r) || Antipodal(q, p) || Antipodal(q, r)) {
    return std::nullopt;
  }
  const double angle = Bearing(q, r) - Bearing(q, p);
  return angle < 0 ? angle + (2 * std::numbers::pi) : angle + 0.0;
}

// A distance over the sphere of S2Earth's radius, measured through the chord, which BigQuery's
// distances match to the last digit.
double Meters(S1ChordAngle distance) { return S2Earth::ToMeters(distance.ToAngle()); }

// The shortest distance between nonempty geographies, which is 0 where they meet.
S1ChordAngle MinDistance(const Geography& a, const Geography& b) {
  MutableS2ShapeIndex index;
  AddShapes(a, index);
  MutableS2ShapeIndex target_index;
  AddShapes(b, target_index);
  S2ClosestEdgeQuery query(&index);
  S2ClosestEdgeQuery::ShapeIndexTarget target(&target_index);
  return query.GetDistance(&target);
}

// The longest distance between nonempty geographies, which BigQuery measures over the edges,
// though ST_MAXDISTANCE describes vertices.
S1ChordAngle MaxDistance(const Geography& a, const Geography& b) {
  MutableS2ShapeIndex index;
  AddShapes(a, index);
  MutableS2ShapeIndex target_index;
  AddShapes(b, target_index);
  S2FurthestEdgeQuery query(&index);
  S2FurthestEdgeQuery::ShapeIndexTarget target(&target_index);
  return query.GetDistance(&target);
}

// BigQuery's error for use_spheroid TRUE where it is not implemented, which `function`, the
// last argument, names. ST_DISTANCE implements it, and the emulator does not.
absl::Status CheckSphere(const Arguments& args, idx_t use_spheroid) {
  if (!args.Bool(use_spheroid)) {
    return absl::OkStatus();
  }
  const std::string function = args.String(use_spheroid + 1);
  if (function == "ST_DISTANCE") {
    return absl::UnimplementedError("The emulator does not support use_spheroid");
  }
  return absl::OutOfRangeError(function + " does not yet implement use_spheroid=true");
}

// ST_DISTANCE and ST_MAXDISTANCE, which the last argument names.
absl::StatusOr<std::optional<double>> Distance(const Arguments& args) {
  GOOGLESQL_ASSIGN_OR_RETURN(const Geography a, ParseGeography(args.String(0)));
  GOOGLESQL_ASSIGN_OR_RETURN(const Geography b, ParseGeography(args.String(1)));
  // Unlike ST_DWITHIN, both distance functions return NULL before checking use_spheroid.
  if (a.Empty() || b.Empty()) {
    return std::nullopt;
  }
  GOOGLESQL_RETURN_IF_ERROR(CheckSphere(args, 2));
  return Meters(args.String(3) == "ST_MAXDISTANCE" ? MaxDistance(a, b) : MinDistance(a, b));
}

// Between single points, the distance itself. Anything else is within the distance where it may be,
// allowing for the error in measuring it: BigQuery takes a point on an edge, a few nanometers
// away as S2 measures it, as within 0 meters, and points that far apart as not.
absl::StatusOr<bool> DWithin(const Arguments& args) {
  GOOGLESQL_RETURN_IF_ERROR(CheckSphere(args, 3));
  GOOGLESQL_ASSIGN_OR_RETURN(const Geography a, ParseGeography(args.String(0)));
  GOOGLESQL_ASSIGN_OR_RETURN(const Geography b, ParseGeography(args.String(1)));
  if (a.Empty() || b.Empty()) {
    return false;
  }
  const double limit = args.Double(2);
  const auto point = [](const Geography& geography) {
    return geography.Count() == 1 && geography.points.size() == 1;
  };
  if (point(a) && point(b)) {
    return Meters(MinDistance(a, b)) <= limit;
  }
  if (!(limit >= 0)) {
    return false;
  }
  MutableS2ShapeIndex index;
  AddShapes(a, index);
  MutableS2ShapeIndex target_index;
  AddShapes(b, target_index);
  S2ClosestEdgeQuery query(&index);
  S2ClosestEdgeQuery::ShapeIndexTarget target(&target_index);
  return query.IsConservativeDistanceLessOrEqual(&target,
                                                 S1ChordAngle(S2Earth::MetersToAngle(limit)));
}

// The discrete Hausdorff distance measures vertices of the first geography to the second's
// edges and interiors; the undirected form takes the maximum in both directions.
absl::StatusOr<std::optional<double>> HausdorffDistance(const Arguments& args) {
  GOOGLESQL_ASSIGN_OR_RETURN(const Geography a, ParseGeography(args.String(0)));
  GOOGLESQL_ASSIGN_OR_RETURN(const Geography b, ParseGeography(args.String(1)));
  MutableS2ShapeIndex index;
  AddShapes(a, index);
  MutableS2ShapeIndex other;
  AddShapes(b, other);
  const S2HausdorffDistanceQuery query;
  const S1ChordAngle distance =
      args.Bool(2) ? query.GetDirectedDistance(&index, &other) : query.GetDistance(&index, &other);
  // S2 returns infinity when no distance exists. BigQuery returns NULL, including where a
  // full polygon has no vertices in a direction being measured.
  if (distance.is_infinity()) {
    return std::nullopt;
  }
  return Meters(distance);
}

// Whether each point of either of `a` and `b`, nonempty, is within a micrometer of the other, as
// Same takes points.
bool Near(const Geography& a, const Geography& b) {
  MutableS2ShapeIndex index;
  AddShapes(a, index);
  MutableS2ShapeIndex other;
  AddShapes(b, other);
  return S2HausdorffDistanceQuery().GetDistance(&index, &other) <=
         S1ChordAngle(S2Earth::MetersToAngle(1e-6));
}

// Geographies are equal where their points, their linestrings and their polygons are: points
// and linestrings where each point of either is within a micrometer of the other, and polygons
// where they cover the same region but for a micrometer. A point is not a linestring, which has
// more than one point, and a linestring is not a polygon, which has an interior.
absl::StatusOr<bool> Equals(const Arguments& args) {
  GOOGLESQL_ASSIGN_OR_RETURN(const Geography a, ParseGeography(args.String(0)));
  GOOGLESQL_ASSIGN_OR_RETURN(const Geography b, ParseGeography(args.String(1)));
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
struct Snapped {
  std::vector<S2Point> points;
  std::vector<std::unique_ptr<S2Polyline>> polylines;
  std::unique_ptr<S2LaxPolygonShape> polygon = std::make_unique<S2LaxPolygonShape>();
};

// Adds `geography` to `builder` in layers of its own that output to `output`.
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

// Adds the shapes of `snapped` to `index`.
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

// Applies `predicate` to `a` and `b` snapped together, so that a vertex of one within a
// micrometer of a vertex or an edge of the other meets it there.
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

// S2's hull can be a thin loop for a line. Normalize its snapped closed set
// so these loops collapse to lines rather than polygons.
absl::StatusOr<std::optional<std::string>> ConvexHull(const Arguments& args) {
  GOOGLESQL_ASSIGN_OR_RETURN(const Geography geography, ParseGeography(args.String(0)));
  if (geography.Empty()) {
    return std::nullopt;
  }
  // BigQuery preserves a single point; S2's artificial tiny loop can move it when snapped.
  if (geography.Count() == 1 && geography.points.size() == 1) {
    return GeographyText(geography);
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
      const Geography result,
      Relate<Geography>(hull, {}, [](const MutableS2ShapeIndex& a, const MutableS2ShapeIndex& b) {
        return ClosedSetOperation(a, b, S2BooleanOperation::OpType::UNION);
      }));
  return GeographyText(result);
}

// Whether `a` and `b` share a point or, with `interiors`, a point of their interiors as OGC
// defines them: a polygon's without its rings, a linestring's without its endpoints unless it
// ends where it starts, and a point's, which is the point.
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

// Whether no point of `b` lies outside `a`, whose rings and endpoints it covers.
bool Cover(const MutableS2ShapeIndex& a, const MutableS2ShapeIndex& b) {
  using Operation = S2BooleanOperation;
  Operation::Options options;
  options.set_polygon_model(Operation::PolygonModel::CLOSED);
  options.set_polyline_model(Operation::PolylineModel::CLOSED);
  return Operation::Contains(a, b, options);
}

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

// What `kPredicate` says of the geographies of the first two arguments.
template <auto kPredicate>
absl::StatusOr<bool> OfGeographies(const Arguments& args) {
  GOOGLESQL_ASSIGN_OR_RETURN(const Geography a, ParseGeography(args.String(0)));
  GOOGLESQL_ASSIGN_OR_RETURN(const Geography b, ParseGeography(args.String(1)));
  return kPredicate(a, b);
}

absl::StatusOr<double> X(const Arguments& args) {
  GOOGLESQL_ASSIGN_OR_RETURN(const S2Point point, ParsePoint(args, 0, "Argument to ST_X"));
  return ToPoint(point).longitude;
}

absl::StatusOr<double> Y(const Arguments& args) {
  GOOGLESQL_ASSIGN_OR_RETURN(const S2Point point, ParsePoint(args, 0, "Argument to ST_Y"));
  return ToPoint(point).latitude;
}

// What `kFunction` returns for the geography of the first argument.
template <auto kFunction>
absl::StatusOr<decltype(kFunction(Geography{}))> OfGeography(const Arguments& args) {
  GOOGLESQL_ASSIGN_OR_RETURN(const Geography geography, ParseGeography(args.String(0)));
  return kFunction(geography);
}

// A closed point set operation after snapping both inputs together.
template <S2BooleanOperation::OpType kOperation>
absl::StatusOr<std::string> SetOperation(const Arguments& args) {
  GOOGLESQL_ASSIGN_OR_RETURN(const Geography a, ParseGeography(args.String(0)));
  GOOGLESQL_ASSIGN_OR_RETURN(const Geography b, ParseGeography(args.String(1)));
  GOOGLESQL_ASSIGN_OR_RETURN(
      const Geography result,
      Relate<Geography>(a, b, [](const MutableS2ShapeIndex& x, const MutableS2ShapeIndex& y) {
        return ClosedSetOperation(x, y, kOperation);
      }));
  return GeographyText(result);
}

absl::StatusOr<std::string> Boundary(const Arguments& args) {
  GOOGLESQL_ASSIGN_OR_RETURN(const Geography geography, ParseGeography(args.String(0)));
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
  GOOGLESQL_ASSIGN_OR_RETURN(const Geography boundary, Combine(parts));
  return GeographyText(boundary);
}

struct BoundingBox {
  double xmin;
  double ymin;
  double xmax;
  double ymax;
};

void SetResult(duckdb_vector output, idx_t row, const BoundingBox& box) {
  const std::array<double, 4> values = {box.xmin, box.ymin, box.xmax, box.ymax};
  for (idx_t i = 0; i < values.size(); ++i) {
    backend_functions::SetResult(duckdb_struct_vector_get_child(output, i), row, values.at(i));
  }
}

std::optional<BoundingBox> Bounds(const Geography& geography) {
  if (geography.Empty()) {
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
  for (const S2Point& point : geography.points) {
    add(S2LatLngRect::FromPoint(S2LatLng(point)));
  }
  for (const std::vector<S2Point>& line : geography.lines) {
    add(S2Polyline(line).GetRectBound());
  }
  if (!geography.polygons.empty()) {
    add(ToS2Polygon(geography)->GetRectBound());
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
  // BigQuery rounds each bound outward by one representable degree value, including zero,
  // whose neighbors are subnormal. Full longitude intervals and the poles stay in range.
  const double infinity = std::numeric_limits<double>::infinity();
  if (gap > 0) {
    box.xmin = std::nextafter(box.xmin, -infinity);
    box.xmax = std::nextafter(box.xmax, infinity);
  }
  box.ymin = std::max(-90.0, std::nextafter(box.ymin, -infinity));
  box.ymax = std::min(90.0, std::nextafter(box.ymax, infinity));
  return box;
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

// Only the highest dimension contributes. S2's centroids already include the
// measure of each component, so their sum gives the weighted centroid.
absl::StatusOr<std::string> Centroid(const Arguments& args) {
  GOOGLESQL_ASSIGN_OR_RETURN(const Geography geography, ParseGeography(args.String(0)));
  if (geography.Empty()) {
    return GeographyText(geography);
  }
  S2Point centroid(0, 0, 0);
  if (!geography.polygons.empty()) {
    centroid = ToS2Polygon(geography)->GetCentroid();
  } else if (!geography.lines.empty()) {
    centroid = std::accumulate(geography.lines.begin(), geography.lines.end(), S2Point(0, 0, 0),
                               [](const S2Point& sum, const std::vector<S2Point>& line) {
                                 return sum + S2::GetCentroid(S2LaxPolylineShape(line));
                               });
  } else {
    centroid = std::accumulate(geography.points.begin(), geography.points.end(), S2Point(0, 0, 0));
  }
  if (centroid.Norm2() == 0) {
    return GeographyText(PointGeography(S2Point(1, 0, 0)));
  }
  return GeographyText(PointGeography(centroid.Normalize()));
}

// ST_LENGTH, the length of the edges of the linestrings, which is 0 for points, polygons and the
// empty geography.
absl::StatusOr<double> Length(const Arguments& args) {
  GOOGLESQL_RETURN_IF_ERROR(CheckSphere(args, 1));
  GOOGLESQL_ASSIGN_OR_RETURN(const Geography geography, ParseGeography(args.String(0)));
  return std::accumulate(geography.lines.begin(), geography.lines.end(), 0.0,
                         [](double length, const std::vector<S2Point>& line) {
                           return length +
                                  S2Earth::ToMeters(S2::GetLength(S2LaxPolylineShape(line)));
                         });
}

// ST_AREA and ST_PERIMETER, which the last argument names, of the polygons over the sphere, and 0
// where there are none.
absl::StatusOr<double> Measure(const Arguments& args) {
  GOOGLESQL_RETURN_IF_ERROR(CheckSphere(args, 1));
  GOOGLESQL_ASSIGN_OR_RETURN(const Geography geography, ParseGeography(args.String(0)));
  if (geography.polygons.empty()) {
    return 0.0;
  }
  const std::unique_ptr<S2Polygon> polygon = ToS2Polygon(geography);
  if (args.String(2) == "ST_AREA") {
    return S2Earth::SteradiansToSquareMeters(polygon->GetArea());
  }
  return S2Earth::ToMeters(S2::GetPerimeter(S2Polygon::Shape(polygon.get())));
}

// BigQuery adds vertices to the GeoJSON of a linestring or a polygon to keep its planar edges
// near the geodesic ones, which the emulator does not.
absl::StatusOr<std::string> AsGeoJson(const Arguments& args) {
  GOOGLESQL_ASSIGN_OR_RETURN(const Geography geography, ParseGeography(args.String(0)));
  if (geography.Full()) {
    return "{ \"type\": \"Polygon\", \"coordinates\": [ [ [180, 90], [-180, 90], [-180, -90], "
           "[180, -90], [180, 90] ] ] } ";
  }
  if (geography.Empty()) {
    return std::string(R"({ "type": "GeometryCollection", "geometries": [ ] } )");
  }
  if (geography.Count() != 1 || geography.points.empty()) {
    std::string type = GeometryType(geography).substr(3);
    std::ranges::transform(type, type.begin(),
                           [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return absl::UnimplementedError("The emulator does not support ST_ASGEOJSON of a " + type);
  }
  const Point point = ToPoint(geography.points.front());
  return R"({ "type": "Point", "coordinates": [)" + Coordinate(point.longitude) + ", " +
         Coordinate(point.latitude) + "] } ";
}

// ST_STARTPOINT, ST_ENDPOINT and ST_POINTN: the vertex of a linestring at a 1-based index, a
// negative one counting from the end.
absl::StatusOr<std::string> PointN(const Arguments& args) {
  GOOGLESQL_ASSIGN_OR_RETURN(const Geography geography, ParseGeography(args.String(0)));
  if (geography.Count() != 1 || geography.lines.empty()) {
    return absl::OutOfRangeError("The input is not a single LINESTRING");
  }
  const std::vector<S2Point>& line = geography.lines.front();
  const int64_t index = args.Int(1);
  const auto size = static_cast<int64_t>(line.size());
  if (index == 0 || index > size || index < -size) {
    return absl::OutOfRangeError("The vertex index " + std::to_string(index) +
                                 " is out of bounds; input LINESTRING has " + Points(line.size()) +
                                 ".");
  }
  return GeographyText(PointGeography(line.at(index > 0 ? index - 1 : size + index)));
}

// BigQuery names the operation and the kind of invalid component in its errors.
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

absl::StatusOr<std::string> LineInterpolatePoint(const Arguments& args) {
  GOOGLESQL_ASSIGN_OR_RETURN(const Geography geography, ParseGeography(args.String(0)));
  const double fraction = args.Double(1);
  if (!std::isfinite(fraction) || fraction < 0 || fraction > 1) {
    return absl::OutOfRangeError(std::format(
        "ST_LINEINTERPOLATEPOINT: ST_LineInterpolatePoint: fraction should be between 0 and 1. "
        "Actual value was {:.6f}; error in ST_LINEINTERPOLATEPOINT expression",
        fraction));
  }
  GOOGLESQL_RETURN_IF_ERROR(
      CheckLine(geography, "ST_LINEINTERPOLATEPOINT", "ST_LineInterpolatePoint", true));
  if (geography.Empty()) {
    return GeographyText(geography);
  }
  return GeographyText(PointGeography(S2Polyline(geography.lines.front()).Interpolate(fraction)));
}

// Project onto the geodesic line, then measure the fraction along its edges.
absl::StatusOr<double> LineLocatePoint(const Arguments& args) {
  GOOGLESQL_ASSIGN_OR_RETURN(const Geography geography, ParseGeography(args.String(0)));
  GOOGLESQL_RETURN_IF_ERROR(
      CheckLine(geography, "ST_LINELOCATEPOINT", "STLineLocatePoint", false, false));
  const auto point = ParsePoint(args, 1, "Argument to STLineLocatePoint");
  if (!point.ok()) {
    return absl::Status(point.status().code(), std::string(point.status().message()) +
                                                   " Error in ST_LINELOCATEPOINT expression");
  }
  const S2Polyline line(geography.lines.front());
  int next_vertex = 0;
  const S2Point projected = line.Project(*point, &next_vertex);
  return line.UnInterpolate(projected, next_vertex);
}

absl::StatusOr<std::string> LineSubstring(const Arguments& args) {
  GOOGLESQL_ASSIGN_OR_RETURN(const Geography geography, ParseGeography(args.String(0)));
  const double start = args.Double(1);
  const double end = args.Double(2);
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
    return GeographyText(PointGeography(first));
  }
  const S2Point last = line.GetSuffix(end, &next_end);
  std::vector<S2Point> vertices{first};
  for (int i = next_start; i < next_end; ++i) {
    vertices.push_back(line.vertex(i));
  }
  if (vertices.back() != last) {
    vertices.push_back(last);
  }
  GOOGLESQL_ASSIGN_OR_RETURN(const Geography result, BuildLine(vertices));
  return GeographyText(result);
}

// ST_UNION, with NULL array elements already removed by the translator.
absl::StatusOr<std::optional<std::string>> UnionGeographies(
    const std::vector<std::string>& geographies) {
  if (geographies.empty()) {
    return std::nullopt;
  }
  std::vector<Geography> parts;
  parts.reserve(geographies.size());
  for (const std::string& text : geographies) {
    GOOGLESQL_ASSIGN_OR_RETURN(Geography geography, ParseGeography(text));
    parts.push_back(std::move(geography));
  }
  GOOGLESQL_ASSIGN_OR_RETURN(const Geography geography, Combine(parts));
  return GeographyText(geography);
}

// The line through the vertices of `geographies`, each a single point or linestring, built as
// ST_GEOGFROMTEXT builds one; a single point is that point.
absl::StatusOr<std::string> MakeLine(const std::vector<std::string>& geographies) {
  std::vector<S2Point> vertices;
  for (const std::string& text : geographies) {
    GOOGLESQL_ASSIGN_OR_RETURN(const Geography geography, ParseGeography(text));
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
    return GeographyText(PointGeography(vertices.front()));
  }
  std::vector<Point> points(vertices.size());
  std::ranges::transform(vertices, points.begin(), ToPoint);
  GOOGLESQL_RETURN_IF_ERROR(CheckAntipodal(points, ""));
  GOOGLESQL_ASSIGN_OR_RETURN(const Geography line, BuildLine(vertices));
  return GeographyText(line);
}

// The polygon whose rings are the linestrings of `geographies`, a shell and then its holes, each
// closed by an edge from its last vertex to its first where it does not end where it starts.
absl::StatusOr<std::string> MakePolygon(const std::vector<std::string>& geographies) {
  std::vector<std::vector<S2Point>> rings;
  for (const std::string& text : geographies) {
    GOOGLESQL_ASSIGN_OR_RETURN(const Geography geography, ParseGeography(text));
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
  GOOGLESQL_ASSIGN_OR_RETURN(const Geography polygon,
                             BuildPolygon(rings, "ST_MakePolygon failed: "));
  return GeographyText(polygon);
}

// A loop of a polygon as a linestring that ends where it starts.
std::string Ring(const std::vector<S2Point>& loop) {
  std::vector<S2Point> line = loop;
  line.push_back(loop.front());
  return GeographyText({.points = {}, .lines = {std::move(line)}, .polygons = {}});
}

// ST_EXTERIORRING and ST_INTERIORRINGS, which `function` names as BigQuery's errors do: the loops
// of a polygon as S2Polygon holds them, a hole enclosing the region it removes. A collection is
// not a polygon, even of one polygon with points or linestrings.
absl::StatusOr<std::vector<std::string>> Rings(const Arguments& args, std::string_view function) {
  GOOGLESQL_ASSIGN_OR_RETURN(const Geography geography, ParseGeography(args.String(0)));
  if (geography.polygons.size() != 1 || geography.Count() != 1) {
    return absl::OutOfRangeError(
        "Argument to " + std::string(function) +
        " must have exactly 1 polygon; argument either had no polygons or more than one.");
  }
  const Polygon& polygon = geography.polygons.front();
  if (geography.Full()) {
    return std::vector<std::string>{GeographyText(Geography{})};
  }
  std::vector<std::string> rings(polygon.size());
  std::ranges::transform(polygon, rings.begin(), Ring);
  return rings;
}

// ST_DUMP: each point, linestring and polygon of a geography, in the order Wkt writes them, of
// the dimension of the second argument, or of any for -1.
absl::StatusOr<std::vector<std::string>> Dump(const Arguments& args) {
  GOOGLESQL_ASSIGN_OR_RETURN(const Geography geography, ParseGeography(args.String(0)));
  const int64_t dimension = args.Int(1);
  if (dimension < -1 || dimension > 2) {
    return absl::OutOfRangeError("Dimension must be -1, 0, 1, or 2, but was " +
                                 std::to_string(dimension) + ". Error in ST_DUMP expression");
  }
  const auto wanted = [&](int64_t of) { return dimension == -1 || dimension == of; };
  std::vector<std::string> parts;
  if (wanted(0)) {
    std::ranges::transform(geography.points, std::back_inserter(parts), [](const S2Point& point) {
      return GeographyText(PointGeography(point));
    });
  }
  if (wanted(1)) {
    std::ranges::transform(geography.lines, std::back_inserter(parts),
                           [](const std::vector<S2Point>& line) {
                             return GeographyText({.points = {}, .lines = {line}, .polygons = {}});
                           });
  }
  if (wanted(2)) {
    std::ranges::transform(
        geography.polygons, std::back_inserter(parts), [](const Polygon& polygon) {
          return GeographyText({.points = {}, .lines = {}, .polygons = {polygon}});
        });
  }
  return parts;
}

constexpr std::string_view kGeohashDigits = "0123456789bcdefghjkmnpqrstuvwxyz";

// A maxchars outside [1, 20] is taken as 20.
absl::StatusOr<std::optional<std::string>> Geohash(const Arguments& args) {
  GOOGLESQL_ASSIGN_OR_RETURN(const Geography geography, ParseGeography(args.String(0)));
  if (geography.Empty()) {
    return std::nullopt;
  }
  GOOGLESQL_ASSIGN_OR_RETURN(const S2Point vertex,
                             ParsePoint(args, 0, "ST_GeoHash: Argument to ST_GeoHash", false));
  const Point point = ToPoint(vertex);
  int64_t length = args.Int(1);
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
absl::StatusOr<std::string> PointFromGeohash(const Arguments& args) {
  const std::string hash = args.String(0);
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
  return GeographyText(PointGeography(ToS2Point({.longitude = middle(0), .latitude = middle(1)})));
}

absl::StatusOr<int64_t> CellIdFromPoint(const Arguments& args) {
  const int64_t level = args.Int(1);
  if (level < 0 || level > S2CellId::kMaxLevel) {
    return absl::OutOfRangeError(
        "S2_CellIdFromPoint: S2CellIdFromPoint failed: level argument must be in range [0,30], "
        "got " +
        std::to_string(level));
  }
  GOOGLESQL_ASSIGN_OR_RETURN(
      const S2Point point,
      ParsePoint(args, 0, "S2_CellIdFromPoint: Argument to S2CellIdFromPoint"));
  return std::bit_cast<int64_t>(S2CellId(point).parent(static_cast<int>(level)).id());
}

}  // namespace

void RegisterGeographyFunctions(duckdb_connection connection) {
  Register(connection, "bq_st_geogpoint", {kDouble, kDouble}, kVarchar, EachRow<GeogPoint>);
  Register(connection, "bq_st_geogfromtext", {kVarchar}, kVarchar, EachRow<GeogFromText>);
  Register(connection, "bq_geography_input", {kVarchar}, kVarchar, EachRow<GeographyInput>);
  Register(connection, "bq_st_geogpointfromgeohash", {kVarchar}, kVarchar,
           EachRow<PointFromGeohash>);
  Register(connection, "bq_st_astext", {kVarchar}, kVarchar,
           EachRow<OfGeography<[](const Geography& geography) { return Wkt(geography); }>>);
  Register(connection, "bq_st_asgeojson", {kVarchar}, kVarchar, EachRow<AsGeoJson>);
  Register(connection, "bq_st_angle", {kVarchar, kVarchar, kVarchar}, kDouble, EachRow<Angle>);
  Register(connection, "bq_st_centroid", {kVarchar}, kVarchar, EachRow<Centroid>);
  Register(connection, "bq_st_convexhull", {kVarchar}, kVarchar, EachRow<ConvexHull>);
  Register(connection, "bq_st_lineinterpolatepoint", {kVarchar, kDouble}, kVarchar,
           EachRow<LineInterpolatePoint>);
  Register(connection, "bq_st_linelocatepoint", {kVarchar, kVarchar}, kDouble,
           EachRow<LineLocatePoint>);
  Register(connection, "bq_st_linesubstring", {kVarchar, kDouble, kDouble}, kVarchar,
           EachRow<LineSubstring>);
  Register(connection, "bq_st_azimuth", {kVarchar, kVarchar}, kDouble, EachRow<Azimuth>);
  Register(connection, "bq_st_distance", {kVarchar, kVarchar, kBoolean, kVarchar}, kDouble,
           EachRow<Distance>);
  Register(connection, "bq_st_hausdorffdistance", {kVarchar, kVarchar, kBoolean}, kDouble,
           EachRow<HausdorffDistance>);
  Register(connection, "bq_st_dwithin", {kVarchar, kVarchar, kDouble, kBoolean, kVarchar}, kBoolean,
           EachRow<DWithin>);
  Register(connection, "bq_st_equals", {kVarchar, kVarchar}, kBoolean, EachRow<Equals>);
  Register(connection, "bq_st_intersects", {kVarchar, kVarchar}, kBoolean,
           EachRow<OfGeographies<Intersects>>);
  Register(connection, "bq_st_intersection", {kVarchar, kVarchar}, kVarchar,
           EachRow<SetOperation<S2BooleanOperation::OpType::INTERSECTION>>);
  Register(connection, "bq_st_difference", {kVarchar, kVarchar}, kVarchar,
           EachRow<SetOperation<S2BooleanOperation::OpType::DIFFERENCE>>);
  Register(connection, "bq_st_boundary", {kVarchar}, kVarchar, EachRow<Boundary>);
  {
    const LogicalType varchar(duckdb_create_logical_type(kVarchar));
    const LogicalType dbl(duckdb_create_logical_type(kDouble));
    std::array<duckdb_logical_type, 4> fields = {dbl.get(), dbl.get(), dbl.get(), dbl.get()};
    std::array<const char*, 4> names = {"xmin", "ymin", "xmax", "ymax"};
    const LogicalType box(duckdb_create_struct_type(fields.data(), names.data(), fields.size()));
    Register(connection, "bq_st_boundingbox", {varchar.get()}, box.get(),
             EachRow<OfGeography<Bounds>>);
  }
  Register(connection, "bq_st_touches", {kVarchar, kVarchar}, kBoolean,
           EachRow<OfGeographies<Touches>>);
  Register(connection, "bq_st_covers", {kVarchar, kVarchar}, kBoolean,
           EachRow<OfGeographies<Covers>>);
  Register(connection, "bq_st_contains", {kVarchar, kVarchar}, kBoolean,
           EachRow<OfGeographies<Contains>>);
  Register(connection, "bq_st_x", {kVarchar}, kDouble, EachRow<X>);
  Register(connection, "bq_st_y", {kVarchar}, kDouble, EachRow<Y>);
  Register(connection, "bq_st_geometrytype", {kVarchar}, kVarchar,
           EachRow<OfGeography<GeometryType>>);
  Register(connection, "bq_st_dimension", {kVarchar}, kBigint, EachRow<OfGeography<Dimension>>);
  Register(connection, "bq_st_numpoints", {kVarchar}, kBigint, EachRow<OfGeography<NumPoints>>);
  Register(connection, "bq_st_isempty", {kVarchar}, kBoolean,
           EachRow<OfGeography<[](const Geography& geography) { return geography.Empty(); }>>);
  Register(connection, "bq_st_iscollection", {kVarchar}, kBoolean,
           EachRow<OfGeography<[](const Geography& geography) { return geography.Count() > 1; }>>);
  Register(connection, "bq_st_numgeometries", {kVarchar}, kBigint,
           EachRow<OfGeography<[](const Geography& geography) {
             return static_cast<int64_t>(geography.Count());
           }>>);
  Register(connection, "bq_st_isclosed", {kVarchar}, kBoolean, EachRow<OfGeography<IsClosed>>);
  Register(connection, "bq_st_isring", {kVarchar}, kBoolean, EachRow<OfGeography<IsRing>>);
  Register(connection, "bq_st_length", {kVarchar, kBoolean, kVarchar}, kDouble, EachRow<Length>);
  Register(connection, "bq_st_measure", {kVarchar, kBoolean, kVarchar}, kDouble, EachRow<Measure>);
  Register(connection, "bq_st_exteriorring", {kVarchar}, kVarchar,
           EachRow<[](const Arguments& args) -> absl::StatusOr<std::string> {
             GOOGLESQL_ASSIGN_OR_RETURN(const std::vector<std::string> rings,
                                        Rings(args, "STExteriorRing"));
             return rings.front();
           }>);
  Register(connection, "bq_st_makepolygon", {kVarchar}, kVarchar,
           EachRow<[](const Arguments& args) { return MakePolygon({args.String(0)}); }>);
  Register(connection, "bq_st_pointn", {kVarchar, kBigint}, kVarchar, EachRow<PointN>);
  Register(
      connection, "bq_st_makeline", {kVarchar, kVarchar}, kVarchar,
      EachRow<[](const Arguments& args) { return MakeLine({args.String(0), args.String(1)}); }>);
  Register(connection, "bq_st_union", {kVarchar, kVarchar}, kVarchar,
           EachRow<[](const Arguments& args) {
             return UnionGeographies({args.String(0), args.String(1)});
           }>);
  {
    const LogicalType varchar(duckdb_create_logical_type(kVarchar));
    const LogicalType list(duckdb_create_list_type(varchar.get()));
    Register(connection, "bq_st_union_array", {list.get()}, varchar.get(),
             EachRow<[](const Arguments& args) { return UnionGeographies(args.Strings(0)); }>);
    Register(connection, "bq_st_makeline_array", {list.get()}, varchar.get(),
             EachRow<[](const Arguments& args) { return MakeLine(args.Strings(0)); }>);
    Register(connection, "bq_st_makepolygon_holes",
             std::vector<duckdb_logical_type>{varchar.get(), list.get()}, varchar.get(),
             EachRow<[](const Arguments& args) {
               std::vector<std::string> rings = args.Strings(1);
               rings.insert(rings.begin(), args.String(0));
               return MakePolygon(rings);
             }>);
    const LogicalType bigint(duckdb_create_logical_type(kBigint));
    Register(connection, "bq_st_dump",
             std::vector<duckdb_logical_type>{varchar.get(), bigint.get()}, list.get(),
             EachRow<Dump>);
    Register(connection, "bq_st_interiorrings", {varchar.get()}, list.get(),
             EachRow<[](const Arguments& args) -> absl::StatusOr<std::vector<std::string>> {
               // BigQuery's error ends as no other function's does.
               auto rings = Rings(args, "STInteriorRings");
               if (!rings.ok()) {
                 return absl::OutOfRangeError(std::string(rings.status().message()) +
                                              " Error in ST_INTERIORRINGS expression");
               }
               rings->erase(rings->begin());
               return *std::move(rings);
             }>);
  }
  Register(connection, "bq_st_geohash", {kVarchar, kBigint}, kVarchar, EachRow<Geohash>);
  Register(connection, "bq_s2_cellidfrompoint", {kVarchar, kBigint}, kBigint,
           EachRow<CellIdFromPoint>);
}

}  // namespace bigquery_emulator_duckdb::backend_functions
