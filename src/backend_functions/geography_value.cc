#include "src/backend_functions/geography_value.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <iterator>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/types/span.h"
#include "googlesql/base/status_macros.h"
#include "s2/mutable_s2shape_index.h"
#include "s2/s1angle.h"
#include "s2/s2builder.h"
#include "s2/s2builderutil_s2point_vector_layer.h"
#include "s2/s2builderutil_s2polyline_vector_layer.h"
#include "s2/s2builderutil_snap_functions.h"
#include "s2/s2closest_edge_query.h"
#include "s2/s2closest_point_query.h"
#include "s2/s2debug.h"
#include "s2/s2earth.h"
#include "s2/s2error.h"
#include "s2/s2latlng.h"
#include "s2/s2lax_loop_shape.h"
#include "s2/s2lax_polyline_shape.h"
#include "s2/s2loop.h"
#include "s2/s2point.h"
#include "s2/s2point_index.h"
#include "s2/s2point_vector_shape.h"
#include "s2/s2pointutil.h"
#include "s2/s2polygon.h"
#include "s2/s2polyline.h"
#include "s2/s2shape.h"

namespace bigquery_emulator_duckdb::backend_functions {
namespace {

constexpr std::string_view kEmpty = "GEOMETRYCOLLECTION EMPTY";

// `vertices` as WKT with the vertices `vertex` writes, separated by commas.
template <typename Vertex>
std::string List(const std::vector<S2Point>& vertices, Vertex vertex) {
  std::string text;
  for (const S2Point& point : vertices) {
    text += (&point == vertices.data() ? "" : ", ") + vertex(point);
  }
  return text;
}

// `geography` as WKT with the vertices `vertex` writes. Unless `stored`, a polygon's rings are
// those of OGC's WKT: closed, a hole running clockwise.
template <typename Vertex>
std::string Text(const Geography& geography, Vertex vertex, bool stored) {
  switch (geography.kind) {
    case Kind::kEmpty:
      return std::string(kEmpty);
    case Kind::kPoint:
      return "POINT(" + List(geography.vertices, vertex) + ")";
    case Kind::kLineString:
      return "LINESTRING(" + List(geography.vertices, vertex) + ")";
    case Kind::kPolygon:
      break;
  }
  std::string text = "POLYGON(";
  for (const std::vector<S2Point>& loop : geography.loops) {
    std::vector<S2Point> ring = loop;
    if (!stored) {
      if (&loop != geography.loops.data()) {
        std::ranges::reverse(ring);
      }
      ring.push_back(ring.front());
    }
    text += (&loop == geography.loops.data() ? "(" : ", (") + List(ring, vertex) + ")";
  }
  return text + ")";
}

// Parses `x y z`, a vertex as GeographyText writes it.
std::optional<S2Point> ReadVertex(std::string_view text) {
  std::array<double, 3> coordinates{};
  const char* position = text.data();
  const char* const end = position + text.size();
  for (double& coordinate : coordinates) {
    if (&coordinate != coordinates.data()) {
      if (position == end || *position != ' ') {
        return std::nullopt;
      }
      ++position;
    }
    const auto result = std::from_chars(position, end, coordinate);
    if (result.ec != std::errc()) {
      return std::nullopt;
    }
    position = result.ptr;
  }
  if (position != end) {
    return std::nullopt;
  }
  return S2Point(coordinates.at(0), coordinates.at(1), coordinates.at(2));
}

// Parses `x y z, ...`, vertices as GeographyText writes them.
std::optional<std::vector<S2Point>> ReadVertices(std::string_view text) {
  std::vector<S2Point> vertices;
  while (true) {
    const size_t comma = text.find(", ");
    const std::optional<S2Point> vertex = ReadVertex(text.substr(0, comma));
    if (!vertex) {
      return std::nullopt;
    }
    vertices.push_back(*vertex);
    if (comma == std::string_view::npos) {
      return vertices;
    }
    text.remove_prefix(comma + 2);
  }
}

}  // namespace

std::string Coordinate(double value, std::optional<int> precision) {
  std::string text(32, '\0');
  char* const end = text.data() + text.size();
  const auto result =
      precision
          ? std::to_chars(text.data(), end, value + 0.0, std::chars_format::general, *precision)
          : std::to_chars(text.data(), end, value + 0.0, std::chars_format::general);
  text.resize(result.ptr - text.data());
  return text;
}

S2Point ToS2Point(const Point& point) {
  return S2LatLng::FromDegrees(point.latitude, point.longitude).ToPoint();
}

Point ToPoint(const S2Point& point) {
  const S2LatLng latlng(point);
  return {.longitude = latlng.lng().degrees(), .latitude = latlng.lat().degrees()};
}

Geography PointGeography(const S2Point& point) {
  return {.kind = Kind::kPoint, .vertices = {point}, .loops = {}};
}

std::string Wkt(const Geography& geography) {
  return Text(
      geography,
      [](const S2Point& vertex) {
        const Point point = ToPoint(vertex);
        return Coordinate(point.longitude) + " " + Coordinate(point.latitude);
      },
      false);
}

std::string GeographyText(const Geography& geography) {
  return Text(
      geography,
      [](const S2Point& vertex) {
        return Coordinate(vertex.x(), std::nullopt) + " " + Coordinate(vertex.y(), std::nullopt) +
               " " + Coordinate(vertex.z(), std::nullopt);
      },
      true);
}

absl::StatusOr<Geography> ParseGeography(std::string_view text) {
  if (text == kEmpty) {
    return Geography{};
  }
  const auto unsupported = [&] {
    return absl::UnimplementedError(
        "The emulator does not support GEOGRAPHY values other than points, linestrings, polygons "
        "or empty geometries: " +
        std::string(text));
  };
  constexpr std::string_view kPolygon = "POLYGON((";
  if (text.starts_with(kPolygon) && text.ends_with("))")) {
    std::string_view rings = text.substr(kPolygon.size(), text.size() - kPolygon.size() - 2);
    Geography polygon{.kind = Kind::kPolygon, .vertices = {}, .loops = {}};
    while (true) {
      const size_t end = rings.find("), (");
      std::optional<std::vector<S2Point>> loop = ReadVertices(rings.substr(0, end));
      if (!loop) {
        return unsupported();
      }
      polygon.loops.push_back(*std::move(loop));
      if (end == std::string_view::npos) {
        return polygon;
      }
      rings.remove_prefix(end + 4);
    }
  }
  for (const auto& [prefix, kind] : {
           std::pair<std::string_view, Kind>{"POINT(", Kind::kPoint},
           {"LINESTRING(", Kind::kLineString},
       }) {
    if (!text.starts_with(prefix) || !text.ends_with(')')) {
      continue;
    }
    std::optional<std::vector<S2Point>> vertices =
        ReadVertices(text.substr(prefix.size(), text.size() - prefix.size() - 1));
    if (vertices) {
      return Geography{.kind = kind, .vertices = *std::move(vertices), .loops = {}};
    }
  }
  return unsupported();
}

std::unique_ptr<S2Shape> Shape(const Geography& geography) {
  if (geography.kind == Kind::kPoint) {
    return std::make_unique<S2PointVectorShape>(geography.vertices);
  }
  if (geography.kind == Kind::kPolygon) {
    return std::make_unique<S2Polygon::OwningShape>(ToS2Polygon(geography));
  }
  return std::make_unique<S2LaxPolylineShape>(geography.vertices);
}

std::unique_ptr<S2Polygon> ToS2Polygon(const Geography& polygon) {
  std::vector<std::unique_ptr<S2Loop>> loops(polygon.loops.size());
  std::ranges::transform(polygon.loops, loops.begin(), [](const std::vector<S2Point>& loop) {
    return std::make_unique<S2Loop>(loop, S2Debug::DISABLE);
  });
  auto result = std::make_unique<S2Polygon>();
  result->set_s2debug_override(S2Debug::DISABLE);
  result->InitNested(std::move(loops));
  return result;
}

bool Same(const S2Point& a, const S2Point& b) {
  return S2::ApproxEquals(a, b, S2Earth::MetersToAngle(1e-6));
}

// Compared by component: S2Point's unary minus trips clang-tidy's analyzer inside S2.
bool Antipodal(const S2Point& p, const S2Point& q) {
  return p.x() == -q.x() && p.y() == -q.y() && p.z() == -q.z();
}

std::string Points(size_t count) {
  return std::to_string(count) + (count == 1 ? " point" : " points");
}

absl::Status CheckAntipodal(const std::vector<Point>& vertices, std::string_view failed) {
  for (size_t i = 0; i + 1 < vertices.size(); ++i) {
    const Point& a = vertices.at(i);
    const Point& b = vertices.at(i + 1);
    if (Antipodal(ToS2Point(a), ToS2Point(b)) ||
        (a.latitude == -b.latitude &&
         (std::abs(a.latitude) == 90 ||
          std::abs(std::remainder(a.longitude - b.longitude, 360.0)) == 180))) {
      return absl::OutOfRangeError(std::string(failed) + "Invalid linestring: Vertices " +
                                   std::to_string(i) + " and " + std::to_string(i + 1) +
                                   " are antipodal");
    }
  }
  return absl::OkStatus();
}

absl::StatusOr<Geography> BuildLine(const std::vector<S2Point>& vertices) {
  std::vector<S2Point> split;
  for (size_t i = 0; i < vertices.size(); ++i) {
    if (i > 0) {
      const S2Point& a = vertices.at(i - 1);
      const S2Point& b = vertices.at(i);
      // BigQuery adds the midpoint of an edge longer than 135 degrees, and splits no half again.
      if (S1Angle(a, b) > S1Angle::Degrees(135)) {
        split.push_back((a + b).Normalize());
      }
    }
    split.push_back(vertices.at(i));
  }
  using Layer = s2builderutil::S2PolylineVectorLayer;
  S2Builder::Options options(s2builderutil::IdentitySnapFunction(S2Earth::MetersToAngle(1e-6)));
  options.set_split_crossing_edges(true);
  S2Builder builder(options);
  Layer::Options layer_options;
  layer_options.set_polyline_type(S2Builder::Graph::PolylineType::WALK);
  layer_options.set_edge_type(S2Builder::EdgeType::UNDIRECTED);
  layer_options.set_duplicate_edges(Layer::Options::DuplicateEdges::MERGE);
  std::vector<std::unique_ptr<S2Polyline>> polylines;
  builder.StartLayer(std::make_unique<Layer>(&polylines, layer_options));
  for (size_t i = 0; i + 1 < split.size(); ++i) {
    builder.AddEdge(split.at(i), split.at(i + 1));
  }
  // The vertices as S2Builder snaps them, which give a line that snaps to one point.
  std::vector<S2Point> sites;
  builder.StartLayer(std::make_unique<s2builderutil::S2PointVectorLayer>(&sites));
  for (const S2Point& vertex : split) {
    builder.AddPoint(vertex);
  }
  S2Error error;
  if (!builder.Build(&error)) {
    return absl::InternalError("S2Builder failed: " + std::string(error.message()));
  }
  if (polylines.empty()) {
    return PointGeography(sites.front());
  }
  if (polylines.size() > 1) {
    return absl::UnimplementedError(
        "The emulator does not support a LINESTRING that BigQuery splits into a "
        "MULTILINESTRING");
  }
  const absl::Span<const S2Point> line = polylines.front()->vertices_span();
  return Geography{.kind = Kind::kLineString, .vertices = {line.begin(), line.end()}, .loops = {}};
}

namespace {

// The rings of `input` without a vertex that repeats the one before it, and with the midpoint of
// an edge longer than 135 degrees, as in a linestring.
absl::StatusOr<std::vector<std::vector<S2Point>>> DistinctRings(
    const std::vector<std::vector<S2Point>>& input, std::string_view failed) {
  std::vector<std::vector<S2Point>> rings;
  for (const std::vector<S2Point>& vertices : input) {
    std::vector<S2Point> distinct;
    std::ranges::unique_copy(vertices, std::back_inserter(distinct));
    while (distinct.size() > 1 && distinct.back() == distinct.front()) {
      distinct.pop_back();
    }
    if (distinct.size() < 3) {
      return absl::OutOfRangeError(std::string(failed) +
                                   "A polygon loop must have at least 3 distinct vertices.");
    }
    std::vector<S2Point>& ring = rings.emplace_back();
    for (size_t i = 0; i < distinct.size(); ++i) {
      const S2Point& a = distinct.at(i);
      const S2Point& b = distinct.at((i + 1) % distinct.size());
      ring.push_back(a);
      if (S1Angle(a, b) > S1Angle::Degrees(135)) {
        ring.push_back((a + b).Normalize());
      }
    }
  }
  return rings;
}

// The loops of distinct rings, failing where a ring crosses itself.
absl::StatusOr<std::vector<std::unique_ptr<S2Loop>>> Loops(
    const std::vector<std::vector<S2Point>>& rings, std::string_view failed) {
  std::vector<std::unique_ptr<S2Loop>> loops;
  for (const std::vector<S2Point>& ring : rings) {
    auto loop = std::make_unique<S2Loop>(ring, S2Debug::DISABLE);
    S2Error error;
    if (loop->FindValidationError(&error)) {
      return absl::OutOfRangeError(std::string(failed) +
                                   "Invalid polygon loop: " + std::string(error.message()));
    }
    loops.push_back(std::move(loop));
  }
  return loops;
}

// The polygon of distinct rings, which must be valid: each encloses the smaller region, which a
// hole removes from the shell's.
absl::StatusOr<Geography> Assemble(const std::vector<std::vector<S2Point>>& rings,
                                   std::string_view failed) {
  GOOGLESQL_ASSIGN_OR_RETURN(std::vector<std::unique_ptr<S2Loop>> loops, Loops(rings, failed));
  std::vector<const S2Loop*> order;
  for (const std::unique_ptr<S2Loop>& loop : loops) {
    loop->Normalize();
    order.push_back(loop.get());
  }
  S2Polygon polygon;
  polygon.set_s2debug_override(S2Debug::DISABLE);
  polygon.InitNested(std::move(loops));
  S2Error error;
  if (polygon.FindValidationError(&error)) {
    return absl::OutOfRangeError(std::string(failed) +
                                 "Invalid polygon: " + std::string(error.message()));
  }
  for (size_t i = 1; i < order.size(); ++i) {
    if (order.at(i)->depth() == 0) {
      return absl::OutOfRangeError(std::string(failed) +
                                   "Polygon should have only one shell, holes must be inside the "
                                   "shell. Polygon hole " +
                                   std::to_string(i) + " is outside of the shell");
    }
  }
  if (order.front()->depth() != 0 ||
      !std::ranges::all_of(order | std::views::drop(1),
                           [](const S2Loop* loop) { return loop->depth() == 1; })) {
    return absl::OutOfRangeError(std::string(failed) +
                                 "Invalid polygon: the holes must lie inside the shell and "
                                 "outside each other");
  }
  Geography result{.kind = Kind::kPolygon, .vertices = {}, .loops = {}};
  for (const S2Loop* loop : order) {
    const absl::Span<const S2Point> vertices = loop->vertices_span();
    result.loops.emplace_back(vertices.begin(), vertices.end());
  }
  return result;
}

// The rings with each vertex moved to the one S2Builder snaps it to with an identity snap function
// of a micrometer, or nullopt where none moves: BigQuery keeps the vertex S2Builder does. Snapping
// that collapses a ring, which BigQuery makes a line, or brings a vertex within a micrometer of an
// edge, which BigQuery snaps too, is unsupported.
absl::StatusOr<std::optional<std::vector<std::vector<S2Point>>>> SnapVertices(
    const std::vector<std::vector<S2Point>>& rings) {
  const S1Angle radius = S2Earth::MetersToAngle(1e-6);
  S2Builder builder{S2Builder::Options(s2builderutil::IdentitySnapFunction(radius))};
  std::vector<S2Point> sites;
  builder.StartLayer(std::make_unique<s2builderutil::S2PointVectorLayer>(&sites));
  for (const std::vector<S2Point>& ring : rings) {
    for (const S2Point& vertex : ring) {
      builder.AddPoint(vertex);
    }
  }
  S2Error error;
  if (!builder.Build(&error)) {
    return absl::InternalError("S2Builder failed: " + std::string(error.message()));
  }
  // S2Builder snaps each vertex to the nearest site.
  S2PointIndex<int> site_index;
  for (const S2Point& site : sites) {
    site_index.Add(site, 0);
  }
  S2ClosestPointQuery<int> site_query(&site_index);
  std::vector<std::vector<S2Point>> snapped = rings;
  bool moved = false;
  for (std::vector<S2Point>& ring : snapped) {
    for (S2Point& vertex : ring) {
      S2ClosestPointQuery<int>::PointTarget target(vertex);
      const S2Point site = site_query.FindClosestPoint(&target).point();
      moved = moved || site != vertex;
      vertex = site;
    }
  }
  if (!moved) {
    return std::nullopt;
  }
  MutableS2ShapeIndex edges;
  for (std::vector<S2Point>& ring : snapped) {
    const auto repeated = std::ranges::unique(ring);
    ring.erase(repeated.begin(), repeated.end());
    while (ring.size() > 1 && ring.back() == ring.front()) {
      ring.pop_back();
    }
    if (ring.size() < 3) {
      return absl::UnimplementedError(
          "The emulator does not support a polygon ring that BigQuery snaps to a line");
    }
    edges.Add(std::make_unique<S2LaxClosedPolylineShape>(ring));
  }
  S2ClosestEdgeQuery edge_query(&edges);
  edge_query.mutable_options()->set_inclusive_max_distance(radius);
  for (const std::vector<S2Point>& ring : snapped) {
    for (const S2Point& vertex : ring) {
      S2ClosestEdgeQuery::PointTarget target(vertex);
      for (const auto& result : edge_query.FindClosestEdges(&target)) {
        const auto edge = edges.shape(result.shape_id())->edge(result.edge_id());
        if (edge.v0 != vertex && edge.v1 != vertex) {
          return absl::UnimplementedError(
              "The emulator does not support a polygon vertex that BigQuery snaps onto an edge");
        }
      }
    }
  }
  return snapped;
}

}  // namespace

absl::StatusOr<Geography> BuildPolygon(const std::vector<std::vector<S2Point>>& input,
                                       std::string_view failed) {
  GOOGLESQL_ASSIGN_OR_RETURN(const std::vector<std::vector<S2Point>> rings,
                             DistinctRings(input, failed));
  // BigQuery checks each ring before snapping, and how the rings lie after.
  GOOGLESQL_RETURN_IF_ERROR(Loops(rings, failed).status());
  GOOGLESQL_ASSIGN_OR_RETURN(const std::optional<std::vector<std::vector<S2Point>>> snapped,
                             SnapVertices(rings));
  absl::StatusOr<Geography> polygon = Assemble(rings, failed);
  if (!snapped) {
    return polygon;
  }
  // Where rings cross or touch at an edge, BigQuery snaps them to the point where they meet.
  if (!polygon.ok()) {
    return absl::UnimplementedError(
        "The emulator does not support a polygon whose rings BigQuery snaps where they meet");
  }
  return Assemble(*snapped, failed);
}

}  // namespace bigquery_emulator_duckdb::backend_functions
