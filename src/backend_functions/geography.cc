#include "src/backend_functions/geography.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
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
#include "s2/s2earth.h"
#include "s2/s2error.h"
#include "s2/s2furthest_edge_query.h"
#include "s2/s2hausdorff_distance_query.h"
#include "s2/s2lax_polygon_shape.h"
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
  if (geography.kind != Kind::kPoint) {
    return absl::OutOfRangeError(
        std::string(context) + " must be a single point geography" +
        (!say                               ? "."
         : geography.kind == Kind::kEmpty   ? "; an argument was the empty geography."
         : geography.kind == Kind::kPolygon ? "; an argument contained a polygon."
                                            : "; an argument contained a line."));
  }
  return geography.vertices.front();
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

// NULL for the same and antipodal points. Otherwise the initial bearing over the sphere, from the
// difference of the longitudes in degrees, which gives nearly antipodal points and those at a pole
// the azimuth BigQuery does, unlike S2Earth::GetInitialBearing.
absl::StatusOr<std::optional<double>> Azimuth(const Arguments& args) {
  GOOGLESQL_ASSIGN_OR_RETURN(const S2Point p,
                             ParsePoint(args, 0, "ST_Azimuth: Argument to STAzimuth"));
  GOOGLESQL_ASSIGN_OR_RETURN(const S2Point q,
                             ParsePoint(args, 1, "ST_Azimuth: Argument to STAzimuth"));
  if (Same(p, q) || Antipodal(p, q)) {
    return std::nullopt;
  }
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

// A distance over the sphere of S2Earth's radius, measured through the chord, which BigQuery's
// distances match to the last digit.
double Meters(S1ChordAngle distance) { return S2Earth::ToMeters(distance.ToAngle()); }

// The shortest distance between nonempty geographies, which is 0 where they meet.
S1ChordAngle MinDistance(const Geography& a, const Geography& b) {
  MutableS2ShapeIndex index;
  index.Add(Shape(a));
  MutableS2ShapeIndex target_index;
  target_index.Add(Shape(b));
  S2ClosestEdgeQuery query(&index);
  S2ClosestEdgeQuery::ShapeIndexTarget target(&target_index);
  return query.GetDistance(&target);
}

// The longest distance between nonempty geographies, which BigQuery measures over the edges,
// though ST_MAXDISTANCE describes vertices.
S1ChordAngle MaxDistance(const Geography& a, const Geography& b) {
  MutableS2ShapeIndex index;
  index.Add(Shape(a));
  MutableS2ShapeIndex target_index;
  target_index.Add(Shape(b));
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
  if (a.kind == Kind::kEmpty || b.kind == Kind::kEmpty) {
    return std::nullopt;
  }
  GOOGLESQL_RETURN_IF_ERROR(CheckSphere(args, 2));
  return Meters(args.String(3) == "ST_MAXDISTANCE" ? MaxDistance(a, b) : MinDistance(a, b));
}

// Between points, the distance itself. A linestring is within the distance where it may be,
// allowing for the error in measuring it: BigQuery takes a point on an edge, a few nanometers
// away as S2 measures it, as within 0 meters, and points that far apart as not.
absl::StatusOr<bool> DWithin(const Arguments& args) {
  GOOGLESQL_RETURN_IF_ERROR(CheckSphere(args, 3));
  GOOGLESQL_ASSIGN_OR_RETURN(const Geography a, ParseGeography(args.String(0)));
  GOOGLESQL_ASSIGN_OR_RETURN(const Geography b, ParseGeography(args.String(1)));
  if (a.kind == Kind::kEmpty || b.kind == Kind::kEmpty) {
    return false;
  }
  const double limit = args.Double(2);
  if (a.kind == Kind::kPoint && b.kind == Kind::kPoint) {
    return Meters(MinDistance(a, b)) <= limit;
  }
  if (!(limit >= 0)) {
    return false;
  }
  MutableS2ShapeIndex index;
  index.Add(Shape(a));
  MutableS2ShapeIndex target_index;
  target_index.Add(Shape(b));
  S2ClosestEdgeQuery query(&index);
  S2ClosestEdgeQuery::ShapeIndexTarget target(&target_index);
  return query.IsConservativeDistanceLessOrEqual(&target,
                                                 S1ChordAngle(S2Earth::MetersToAngle(limit)));
}

// Geographies are equal where each point of either is within a micrometer of the other, as Same
// takes points, and polygons where they cover the same region but for a micrometer. Geographies
// of different kinds differ: a point is not a linestring, which has more than one point, and a
// linestring is not a polygon, which has an interior.
absl::StatusOr<bool> Equals(const Arguments& args) {
  GOOGLESQL_ASSIGN_OR_RETURN(const Geography a, ParseGeography(args.String(0)));
  GOOGLESQL_ASSIGN_OR_RETURN(const Geography b, ParseGeography(args.String(1)));
  if (a.kind != b.kind) {
    return false;
  }
  switch (a.kind) {
    case Kind::kEmpty:
      return true;
    case Kind::kPoint:
      return Same(a.vertices.front(), b.vertices.front());
    case Kind::kPolygon:
      return ToS2Polygon(a)->ApproxEquals(*ToS2Polygon(b), S2Earth::MetersToAngle(1e-6));
    case Kind::kLineString:
      break;
  }
  MutableS2ShapeIndex index;
  index.Add(Shape(a));
  MutableS2ShapeIndex other;
  other.Add(Shape(b));
  return S2HausdorffDistanceQuery().GetDistance(&index, &other) <=
         S1ChordAngle(S2Earth::MetersToAngle(1e-6));
}

// A geography as S2Builder outputs it, in the layer for its kind.
struct Snapped {
  std::vector<S2Point> points;
  std::vector<std::unique_ptr<S2Polyline>> polylines;
  std::unique_ptr<S2LaxPolygonShape> polygon = std::make_unique<S2LaxPolygonShape>();
};

// Adds nonempty `geography` to `builder` in a layer of its own that outputs to `output`.
void AddLayer(S2Builder& builder, const Geography& geography, Snapped& output) {
  switch (geography.kind) {
    case Kind::kEmpty:
      break;
    case Kind::kPoint:
      builder.StartLayer(std::make_unique<s2builderutil::S2PointVectorLayer>(&output.points));
      builder.AddPoint(geography.vertices.front());
      break;
    case Kind::kLineString: {
      // One walk, which gains no endpoints where a vertex is added.
      s2builderutil::S2PolylineVectorLayer::Options options;
      options.set_polyline_type(S2Builder::Graph::PolylineType::WALK);
      builder.StartLayer(
          std::make_unique<s2builderutil::S2PolylineVectorLayer>(&output.polylines, options));
      builder.AddPolyline(geography.vertices);
      break;
    }
    case Kind::kPolygon:
      builder.StartLayer(std::make_unique<s2builderutil::LaxPolygonLayer>(output.polygon.get()));
      builder.AddPolygon(*ToS2Polygon(geography));
      break;
  }
}

// Adds the shapes of `snapped` to `index`.
void AddShapes(Snapped snapped, MutableS2ShapeIndex& index) {
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

// Whether `predicate` holds for nonempty `a` and `b` as BigQuery's predicates take them: snapped
// together, so that a vertex of one within a micrometer of a vertex or an edge of the other
// meets it there.
template <typename Predicate>
absl::StatusOr<bool> Relate(const Geography& a, const Geography& b, Predicate predicate) {
  S2Builder builder{
      S2Builder::Options(s2builderutil::IdentitySnapFunction(S2Earth::MetersToAngle(1e-6)))};
  Snapped snapped_a;
  Snapped snapped_b;
  AddLayer(builder, a, snapped_a);
  AddLayer(builder, b, snapped_b);
  S2Error error;
  if (!builder.Build(&error)) {
    return absl::InternalError("S2Builder failed: " + std::string(error.message()));
  }
  MutableS2ShapeIndex index_a;
  AddShapes(std::move(snapped_a), index_a);
  MutableS2ShapeIndex index_b;
  AddShapes(std::move(snapped_b), index_b);
  return predicate(index_a, index_b);
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
  if (a.kind == Kind::kEmpty || b.kind == Kind::kEmpty) {
    return false;
  }
  return Relate(a, b, [](const MutableS2ShapeIndex& x, const MutableS2ShapeIndex& y) {
    return Meet(x, y, false);
  });
}

// The geographies meet only where the boundary of one is.
absl::StatusOr<bool> Touches(const Geography& a, const Geography& b) {
  if (a.kind == Kind::kEmpty || b.kind == Kind::kEmpty) {
    return false;
  }
  return Relate(a, b, [](const MutableS2ShapeIndex& x, const MutableS2ShapeIndex& y) {
    return Meet(x, y, false) && !Meet(x, y, true);
  });
}

absl::StatusOr<bool> Covers(const Geography& a, const Geography& b) {
  if (a.kind == Kind::kEmpty || b.kind == Kind::kEmpty) {
    return false;
  }
  return Relate(a, b, Cover);
}

// `a` covers `b`, and their interiors meet, so a geography does not contain its boundary.
absl::StatusOr<bool> Contains(const Geography& a, const Geography& b) {
  if (a.kind == Kind::kEmpty || b.kind == Kind::kEmpty) {
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

// The dimension of the empty geography, a point, a linestring and a polygon.
int64_t Dimension(const Geography& geography) {
  switch (geography.kind) {
    case Kind::kEmpty:
      return -1;
    case Kind::kPoint:
      return 0;
    case Kind::kLineString:
      return 1;
    case Kind::kPolygon:
      return 2;
  }
  return -1;
}

std::string GeometryType(const Geography& geography) {
  constexpr std::array<std::string_view, 4> kTypes = {
      "ST_GeometryCollection",
      "ST_Point",
      "ST_LineString",
      "ST_Polygon",
  };
  return std::string(kTypes.at(Dimension(geography) + 1));
}

// A polygon's rings count their first vertex twice, as WKT writes them.
int64_t NumPoints(const Geography& geography) {
  return static_cast<int64_t>(std::accumulate(
      geography.loops.begin(), geography.loops.end(), geography.vertices.size(),
      [](size_t count, const std::vector<S2Point>& loop) { return count + loop.size() + 1; }));
}

// A point is closed, and a linestring where it ends where it starts. Only the full polygon, which
// the emulator does not hold, is closed.
bool IsClosed(const Geography& geography) {
  return (geography.kind == Kind::kPoint || geography.kind == Kind::kLineString) &&
         Same(geography.vertices.front(), geography.vertices.back());
}

// ST_LENGTH, the length of a linestring's edges, which is 0 for a point or an empty geography.
absl::StatusOr<double> Length(const Arguments& args) {
  GOOGLESQL_RETURN_IF_ERROR(CheckSphere(args, 1));
  GOOGLESQL_ASSIGN_OR_RETURN(const Geography geography, ParseGeography(args.String(0)));
  return geography.kind != Kind::kLineString ? 0.0
                                             : S2Earth::ToMeters(S2::GetLength(*Shape(geography)));
}

// ST_AREA and ST_PERIMETER, which the last argument names, of a polygon over the sphere, and 0
// for anything else.
absl::StatusOr<double> Measure(const Arguments& args) {
  GOOGLESQL_RETURN_IF_ERROR(CheckSphere(args, 1));
  GOOGLESQL_ASSIGN_OR_RETURN(const Geography geography, ParseGeography(args.String(0)));
  if (geography.kind != Kind::kPolygon) {
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
  if (geography.kind == Kind::kEmpty) {
    return std::string(R"({ "type": "GeometryCollection", "geometries": [ ] } )");
  }
  if (geography.kind == Kind::kLineString) {
    return absl::UnimplementedError("The emulator does not support ST_ASGEOJSON of a LINESTRING");
  }
  if (geography.kind == Kind::kPolygon) {
    return absl::UnimplementedError("The emulator does not support ST_ASGEOJSON of a POLYGON");
  }
  const Point point = ToPoint(geography.vertices.front());
  return R"({ "type": "Point", "coordinates": [)" + Coordinate(point.longitude) + ", " +
         Coordinate(point.latitude) + "] } ";
}

// ST_STARTPOINT, ST_ENDPOINT and ST_POINTN: the vertex of a linestring at a 1-based index, a
// negative one counting from the end.
absl::StatusOr<std::string> PointN(const Arguments& args) {
  GOOGLESQL_ASSIGN_OR_RETURN(const Geography line, ParseGeography(args.String(0)));
  const int64_t index = args.Int(1);
  const auto size = static_cast<int64_t>(line.vertices.size());
  if (line.kind != Kind::kLineString) {
    return absl::OutOfRangeError("The input is not a single LINESTRING");
  }
  if (index == 0 || index > size || index < -size) {
    return absl::OutOfRangeError("The vertex index " + std::to_string(index) +
                                 " is out of bounds; input LINESTRING has " +
                                 Points(line.vertices.size()) + ".");
  }
  return GeographyText(PointGeography(line.vertices.at(index > 0 ? index - 1 : size + index)));
}

// The line through the vertices of the points and linestrings of `geographies`, built as
// ST_GEOGFROMTEXT builds one; a single point is that point.
absl::StatusOr<std::string> MakeLine(const std::vector<std::string>& geographies) {
  std::vector<S2Point> vertices;
  for (const std::string& text : geographies) {
    GOOGLESQL_ASSIGN_OR_RETURN(const Geography geography, ParseGeography(text));
    if (geography.kind == Kind::kEmpty || geography.kind == Kind::kPolygon) {
      return absl::OutOfRangeError("ST_MAKELINE arguments must be single points or lines");
    }
    vertices.insert(vertices.end(), geography.vertices.begin(), geography.vertices.end());
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
    if (geography.kind == Kind::kEmpty) {
      return absl::UnimplementedError(
          "The emulator does not support ST_MAKEPOLYGON of an empty GEOGRAPHY, which makes the "
          "full polygon");
    }
    if (geography.kind != Kind::kLineString) {
      return absl::OutOfRangeError(
          "ST_MakePolygon failed: Input geographies for STMakePolygon must be empty or consist of "
          "exactly one linestring.");
    }
    std::vector<S2Point>& ring = rings.emplace_back(geography.vertices);
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
  Geography line{.kind = Kind::kLineString, .vertices = loop, .loops = {}};
  line.vertices.push_back(loop.front());
  return GeographyText(line);
}

// ST_EXTERIORRING and ST_INTERIORRINGS, which `function` names as BigQuery's errors do: the loops
// of a polygon as S2Polygon holds them, a hole enclosing the region it removes.
absl::StatusOr<std::vector<std::string>> Rings(const Arguments& args, std::string_view function) {
  GOOGLESQL_ASSIGN_OR_RETURN(const Geography polygon, ParseGeography(args.String(0)));
  if (polygon.kind != Kind::kPolygon) {
    return absl::OutOfRangeError(
        "Argument to " + std::string(function) +
        " must have exactly 1 polygon; argument either had no polygons or more than one.");
  }
  std::vector<std::string> rings(polygon.loops.size());
  std::ranges::transform(polygon.loops, rings.begin(), Ring);
  return rings;
}

constexpr std::string_view kGeohashDigits = "0123456789bcdefghjkmnpqrstuvwxyz";

// A maxchars outside [1, 20] is taken as 20.
absl::StatusOr<std::optional<std::string>> Geohash(const Arguments& args) {
  GOOGLESQL_ASSIGN_OR_RETURN(const Geography geography, ParseGeography(args.String(0)));
  if (geography.kind == Kind::kEmpty) {
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
  Register(connection, "bq_st_geogpointfromgeohash", {kVarchar}, kVarchar,
           EachRow<PointFromGeohash>);
  Register(connection, "bq_st_astext", {kVarchar}, kVarchar,
           EachRow<OfGeography<[](const Geography& geography) { return Wkt(geography); }>>);
  Register(connection, "bq_st_asgeojson", {kVarchar}, kVarchar, EachRow<AsGeoJson>);
  Register(connection, "bq_st_azimuth", {kVarchar, kVarchar}, kDouble, EachRow<Azimuth>);
  Register(connection, "bq_st_distance", {kVarchar, kVarchar, kBoolean, kVarchar}, kDouble,
           EachRow<Distance>);
  Register(connection, "bq_st_dwithin", {kVarchar, kVarchar, kDouble, kBoolean, kVarchar}, kBoolean,
           EachRow<DWithin>);
  Register(connection, "bq_st_equals", {kVarchar, kVarchar}, kBoolean, EachRow<Equals>);
  Register(connection, "bq_st_intersects", {kVarchar, kVarchar}, kBoolean,
           EachRow<OfGeographies<Intersects>>);
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
  Register(
      connection, "bq_st_isempty", {kVarchar}, kBoolean,
      EachRow<
          OfGeography<[](const Geography& geography) { return geography.kind == Kind::kEmpty; }>>);
  // A point, a linestring or a polygon is never a collection.
  Register(connection, "bq_st_iscollection", {kVarchar}, kBoolean,
           EachRow<OfGeography<[](const Geography&) { return false; }>>);
  Register(connection, "bq_st_isclosed", {kVarchar}, kBoolean, EachRow<OfGeography<IsClosed>>);
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
  {
    const LogicalType varchar(duckdb_create_logical_type(kVarchar));
    const LogicalType list(duckdb_create_list_type(varchar.get()));
    Register(connection, "bq_st_makeline_array", {list.get()}, varchar.get(),
             EachRow<[](const Arguments& args) { return MakeLine(args.Strings(0)); }>);
    Register(connection, "bq_st_makepolygon_holes",
             std::vector<duckdb_logical_type>{varchar.get(), list.get()}, varchar.get(),
             EachRow<[](const Arguments& args) {
               std::vector<std::string> rings = args.Strings(1);
               rings.insert(rings.begin(), args.String(0));
               return MakePolygon(rings);
             }>);
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
