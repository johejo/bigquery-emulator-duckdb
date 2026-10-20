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

#include "absl/container/flat_hash_set.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/types/span.h"
#include "googlesql/base/status_macros.h"
#include "s2/mutable_s2shape_index.h"
#include "s2/s1angle.h"
#include "s2/s2boolean_operation.h"
#include "s2/s2builder.h"
#include "s2/s2builderutil_s2point_vector_layer.h"
#include "s2/s2builderutil_s2polygon_layer.h"
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

// `items` separated by commas.
std::string Join(const std::vector<std::string>& items) {
  std::string text;
  for (const std::string& item : items) {
    text += (&item == items.data() ? "" : ", ") + item;
  }
  return text;
}

// `vertices` as WKT with the vertices `vertex` writes, separated by commas.
template <typename Vertex>
std::string List(const std::vector<S2Point>& vertices, Vertex vertex) {
  std::vector<std::string> items(vertices.size());
  std::ranges::transform(vertices, items.begin(), vertex);
  return Join(items);
}

// A WKT type and the MULTI type of more than one of it.
struct Types {
  std::string_view one;
  std::string_view many;
};

// `parts` as WKT of their type, each as `part` writes it.
template <typename Item, typename Part>
std::string Parts(const std::vector<Item>& parts, Types types, Part part) {
  if (parts.size() == 1) {
    return std::string(types.one) + part(parts.front());
  }
  std::vector<std::string> items(parts.size());
  std::ranges::transform(parts, items.begin(), part);
  return std::string(types.many) + "(" + Join(items) + ")";
}

// `geography` as WKT with the vertices `vertex` writes. Unless `stored`, a polygon's rings are
// those of OGC's WKT: closed, a hole running clockwise.
template <typename Vertex>
std::string Text(const Geography& geography, Vertex vertex, bool stored) {
  if (geography.Empty()) {
    return std::string(kEmpty);
  }
  const auto vertices = [&](const std::vector<S2Point>& line) {
    return "(" + List(line, vertex) + ")";
  };
  const auto rings = [&](const Polygon& polygon) {
    std::vector<std::string> items;
    for (const std::vector<S2Point>& loop : polygon) {
      std::vector<S2Point> ring = loop;
      if (!stored) {
        if (&loop != polygon.data()) {
          std::ranges::reverse(ring);
        }
        ring.push_back(ring.front());
      }
      items.push_back(vertices(ring));
    }
    return "(" + Join(items) + ")";
  };
  std::vector<std::string> parts;
  if (!geography.points.empty()) {
    parts.push_back(geography.points.size() == 1
                        ? "POINT(" + vertex(geography.points.front()) + ")"
                        : "MULTIPOINT(" + List(geography.points, vertex) + ")");
  }
  if (!geography.lines.empty()) {
    parts.push_back(
        Parts(geography.lines, {.one = "LINESTRING", .many = "MULTILINESTRING"}, vertices));
  }
  if (!geography.polygons.empty()) {
    parts.push_back(Parts(geography.polygons, {.one = "POLYGON", .many = "MULTIPOLYGON"}, rings));
  }
  return parts.size() == 1 ? parts.front() : "GEOMETRYCOLLECTION(" + Join(parts) + ")";
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

// The items of `text` that commas outside parentheses separate.
std::vector<std::string_view> Items(std::string_view text) {
  std::vector<std::string_view> items;
  size_t depth = 0;
  size_t start = 0;
  for (size_t i = 0; i < text.size(); ++i) {
    if (text.at(i) == '(') {
      ++depth;
    } else if (text.at(i) == ')') {
      --depth;
    } else if (text.at(i) == ',' && depth == 0) {
      items.push_back(text.substr(start, i - start));
      start = i + 2;
    }
  }
  items.push_back(text.substr(start));
  return items;
}

// What `text` holds in the parentheses after `name`.
std::optional<std::string_view> Inside(std::string_view text, std::string_view name) {
  if (!text.starts_with(name) || text.size() < name.size() + 2 || text.at(name.size()) != '(' ||
      !text.ends_with(')')) {
    return std::nullopt;
  }
  return text.substr(name.size() + 1, text.size() - name.size() - 2);
}

// Parses `(x y z, ...)`, vertices as GeographyText writes them.
std::optional<std::vector<S2Point>> ReadVertices(std::string_view text) {
  const std::optional<std::string_view> inside = Inside(text, "");
  if (!inside) {
    return std::nullopt;
  }
  std::vector<S2Point> vertices;
  for (const std::string_view item : Items(*inside)) {
    const std::optional<S2Point> vertex = ReadVertex(item);
    if (!vertex) {
      return std::nullopt;
    }
    vertices.push_back(*vertex);
  }
  return vertices;
}

// Parses `((x y z, ...), ...)`, the rings of a polygon as GeographyText writes them.
std::optional<Polygon> ReadPolygon(std::string_view text) {
  const std::optional<std::string_view> inside = Inside(text, "");
  if (!inside) {
    return std::nullopt;
  }
  Polygon polygon;
  for (const std::string_view item : Items(*inside)) {
    std::optional<std::vector<S2Point>> loop = ReadVertices(item);
    if (!loop) {
      return std::nullopt;
    }
    polygon.push_back(*std::move(loop));
  }
  return polygon;
}

// Adds the part of a geography `text` holds to `geography`, returning whether it was one.
bool ReadPart(std::string_view text, Geography& geography) {
  if (const auto point = Inside(text, "POINT")) {
    const std::optional<S2Point> vertex = ReadVertex(*point);
    if (vertex) {
      geography.points.push_back(*vertex);
    }
    return vertex.has_value();
  }
  if (const auto points = Inside(text, "MULTIPOINT")) {
    for (const std::string_view item : Items(*points)) {
      const std::optional<S2Point> vertex = ReadVertex(item);
      if (!vertex) {
        return false;
      }
      geography.points.push_back(*vertex);
    }
    return true;
  }
  if (text.starts_with("LINESTRING(")) {
    std::optional<std::vector<S2Point>> line = ReadVertices(text.substr(10));
    if (!line) {
      return false;
    }
    geography.lines.push_back(*std::move(line));
    return true;
  }
  if (const auto lines = Inside(text, "MULTILINESTRING")) {
    for (const std::string_view item : Items(*lines)) {
      std::optional<std::vector<S2Point>> line = ReadVertices(item);
      if (!line) {
        return false;
      }
      geography.lines.push_back(*std::move(line));
    }
    return true;
  }
  if (text.starts_with("POLYGON(")) {
    std::optional<Polygon> polygon = ReadPolygon(text.substr(7));
    if (!polygon) {
      return false;
    }
    geography.polygons.push_back(*std::move(polygon));
    return true;
  }
  if (const auto polygons = Inside(text, "MULTIPOLYGON")) {
    for (const std::string_view item : Items(*polygons)) {
      std::optional<Polygon> polygon = ReadPolygon(item);
      if (!polygon) {
        return false;
      }
      geography.polygons.push_back(*std::move(polygon));
    }
    return true;
  }
  return false;
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
  return {.points = {point}, .lines = {}, .polygons = {}};
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
  Geography geography;
  if (text == kEmpty) {
    return geography;
  }
  const std::optional<std::string_view> collection = Inside(text, "GEOMETRYCOLLECTION");
  const std::vector<std::string_view> parts =
      collection ? Items(*collection) : std::vector<std::string_view>{text};
  if (!std::ranges::all_of(parts,
                           [&](std::string_view part) { return ReadPart(part, geography); })) {
    return absl::InvalidArgumentError("Invalid GEOGRAPHY value: " + std::string(text));
  }
  return geography;
}

void AddShapes(const Geography& geography, MutableS2ShapeIndex& index) {
  if (!geography.points.empty()) {
    index.Add(std::make_unique<S2PointVectorShape>(geography.points));
  }
  for (const std::vector<S2Point>& line : geography.lines) {
    index.Add(std::make_unique<S2LaxPolylineShape>(line));
  }
  if (!geography.polygons.empty()) {
    index.Add(std::make_unique<S2Polygon::OwningShape>(ToS2Polygon(geography)));
  }
}

std::unique_ptr<S2Polygon> ToS2Polygon(const Geography& geography) {
  std::vector<std::unique_ptr<S2Loop>> loops;
  for (const Polygon& polygon : geography.polygons) {
    std::ranges::transform(polygon, std::back_inserter(loops),
                           [](const std::vector<S2Point>& loop) {
                             return std::make_unique<S2Loop>(loop, S2Debug::DISABLE);
                           });
  }
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
  Geography result;
  for (const std::unique_ptr<S2Polyline>& polyline : polylines) {
    const absl::Span<const S2Point> line = polyline->vertices_span();
    result.lines.emplace_back(line.begin(), line.end());
  }
  return result;
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
  Polygon result;
  for (const S2Loop* loop : order) {
    const absl::Span<const S2Point> vertices = loop->vertices_span();
    result.emplace_back(vertices.begin(), vertices.end());
  }
  return Geography{.points = {}, .lines = {}, .polygons = {std::move(result)}};
}

// The rings with each vertex moved to the one S2Builder snaps it to with an identity snap function
// of a micrometer, or nullopt where none moves: BigQuery keeps the vertex S2Builder does. Snapping
// that collapses a shell with holes, or brings a vertex within a micrometer of an edge, which
// BigQuery snaps too, is unsupported.
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
  for (std::vector<S2Point>& ring : snapped) {
    const auto repeated = std::ranges::unique(ring);
    ring.erase(repeated.begin(), repeated.end());
    while (ring.size() > 1 && ring.back() == ring.front()) {
      ring.pop_back();
    }
  }
  // A hole that collapses is dropped, and a shell without holes that collapses is the line or the
  // point it leaves.
  std::erase_if(snapped, [&](const std::vector<S2Point>& ring) {
    return &ring != snapped.data() && ring.size() < 3;
  });
  if (snapped.front().size() < 3) {
    if (snapped.size() == 1) {
      return snapped;
    }
    return absl::UnimplementedError(
        "The emulator does not support a polygon shell that BigQuery snaps to a line or a point "
        "while it has holes");
  }
  MutableS2ShapeIndex edges;
  for (const std::vector<S2Point>& ring : snapped) {
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
  // BigQuery checks and normalizes each ring before snapping, and checks how the rings lie after.
  GOOGLESQL_ASSIGN_OR_RETURN(const std::vector<std::unique_ptr<S2Loop>> loops,
                             Loops(rings, failed));
  std::vector<std::vector<S2Point>> normalized;
  for (const std::unique_ptr<S2Loop>& loop : loops) {
    loop->Normalize();
    const absl::Span<const S2Point> vertices = loop->vertices_span();
    normalized.emplace_back(vertices.begin(), vertices.end());
  }
  GOOGLESQL_ASSIGN_OR_RETURN(const std::optional<std::vector<std::vector<S2Point>>> snapped,
                             SnapVertices(normalized));
  absl::StatusOr<Geography> polygon = Assemble(rings, failed);
  if (!snapped) {
    return polygon;
  }
  // Where rings cross or touch at an edge, BigQuery snaps them to the point where they meet.
  if (!polygon.ok()) {
    return absl::UnimplementedError(
        "The emulator does not support a polygon whose rings BigQuery snaps where they meet");
  }
  if (snapped->front().size() == 1) {
    return PointGeography(snapped->front().front());
  }
  if (snapped->front().size() == 2) {
    return BuildLine(snapped->front());
  }
  return Assemble(*snapped, failed);
}

namespace {

// The output of a union, which the next one reads.
struct Union {
  std::vector<S2Point> points;
  std::vector<std::unique_ptr<S2Polyline>> lines;
  S2Polygon polygon;
};

// The geography of `output`, whose points on a linestring or a polygon's ring, which S2 keeps
// unless they are its vertices, are dropped as BigQuery drops them.
Geography Read(const Union& output) {
  Geography geography;
  absl::flat_hash_set<S2Point> vertices;
  for (const std::unique_ptr<S2Polyline>& polyline : output.lines) {
    const absl::Span<const S2Point> line = polyline->vertices_span();
    geography.lines.emplace_back(line.begin(), line.end());
    vertices.insert(line.begin(), line.end());
  }
  // S2Polygon holds its loops in preorder: each shell is followed by its holes and what lies in
  // them.
  std::vector<size_t> polygon_of(output.polygon.num_loops());
  for (int i = 0; i < output.polygon.num_loops(); ++i) {
    const S2Loop* loop = output.polygon.loop(i);
    const absl::Span<const S2Point> ring = loop->vertices_span();
    vertices.insert(ring.begin(), ring.end());
    if (loop->is_hole()) {
      polygon_of.at(i) = polygon_of.at(output.polygon.GetParent(i));
    } else {
      polygon_of.at(i) = geography.polygons.size();
      geography.polygons.emplace_back();
    }
    geography.polygons.at(polygon_of.at(i)).emplace_back(ring.begin(), ring.end());
  }
  std::ranges::copy_if(output.points, std::back_inserter(geography.points),
                       [&](const S2Point& point) { return !vertices.contains(point); });
  return geography;
}

}  // namespace

absl::StatusOr<Geography> Combine(const std::vector<Geography>& parts) {
  if (parts.size() == 1) {
    return parts.front();
  }
  // The points come first, all at once, and then each linestring and polygon.
  std::vector<std::unique_ptr<MutableS2ShapeIndex>> inputs;
  Geography points;
  for (const Geography& part : parts) {
    points.points.insert(points.points.end(), part.points.begin(), part.points.end());
  }
  if (!points.Empty()) {
    AddShapes(points, *inputs.emplace_back(std::make_unique<MutableS2ShapeIndex>()));
  }
  for (const Geography& part : parts) {
    for (const std::vector<S2Point>& line : part.lines) {
      AddShapes({.points = {}, .lines = {line}, .polygons = {}},
                *inputs.emplace_back(std::make_unique<MutableS2ShapeIndex>()));
    }
    for (const Polygon& polygon : part.polygons) {
      AddShapes({.points = {}, .lines = {}, .polygons = {polygon}},
                *inputs.emplace_back(std::make_unique<MutableS2ShapeIndex>()));
    }
  }
  const S2BooleanOperation::Options options(
      s2builderutil::IdentitySnapFunction(S2Earth::MetersToAngle(1e-6)));
  using PolylineLayer = s2builderutil::S2PolylineVectorLayer;
  PolylineLayer::Options line_options;
  line_options.set_polyline_type(S2Builder::Graph::PolylineType::WALK);
  line_options.set_edge_type(S2Builder::EdgeType::UNDIRECTED);
  line_options.set_duplicate_edges(PolylineLayer::Options::DuplicateEdges::MERGE);
  auto output = std::make_unique<Union>();
  auto index = std::make_unique<MutableS2ShapeIndex>();
  for (const std::unique_ptr<MutableS2ShapeIndex>& input : inputs) {
    auto next = std::make_unique<Union>();
    std::vector<std::unique_ptr<S2Builder::Layer>> layers;
    layers.push_back(std::make_unique<s2builderutil::S2PointVectorLayer>(&next->points));
    layers.push_back(std::make_unique<PolylineLayer>(&next->lines, line_options));
    layers.push_back(std::make_unique<s2builderutil::S2PolygonLayer>(&next->polygon));
    S2BooleanOperation operation(S2BooleanOperation::OpType::UNION, std::move(layers), options);
    S2Error error;
    if (!operation.Build(*index, *input, &error)) {
      return absl::InternalError("S2BooleanOperation failed: " + std::string(error.message()));
    }
    // The next index reads the shapes of this output, which outlives it.
    auto next_index = std::make_unique<MutableS2ShapeIndex>();
    if (!next->points.empty()) {
      next_index->Add(std::make_unique<S2PointVectorShape>(next->points));
    }
    for (const std::unique_ptr<S2Polyline>& line : next->lines) {
      next_index->Add(std::make_unique<S2Polyline::Shape>(line.get()));
    }
    if (next->polygon.num_loops() > 0) {
      next_index->Add(std::make_unique<S2Polygon::Shape>(&next->polygon));
    }
    index = std::move(next_index);
    output = std::move(next);
  }
  return Read(*output);
}

}  // namespace bigquery_emulator_duckdb::backend_functions
