#include "src/googlesql_geography/value.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <iterator>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_set.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/types/span.h"
#include "googlesql/base/status_macros.h"
#include "s2/mutable_s2shape_index.h"
#include "s2/r2.h"
#include "s2/s1angle.h"
#include "s2/s2boolean_operation.h"
#include "s2/s2buffer_operation.h"
#include "s2/s2builder.h"
#include "s2/s2builderutil_closed_set_normalizer.h"
#include "s2/s2builderutil_s2point_vector_layer.h"
#include "s2/s2builderutil_s2polygon_layer.h"
#include "s2/s2builderutil_s2polyline_vector_layer.h"
#include "s2/s2builderutil_snap_functions.h"
#include "s2/s2debug.h"
#include "s2/s2earth.h"
#include "s2/s2error.h"
#include "s2/s2latlng.h"
#include "s2/s2lax_polyline_shape.h"
#include "s2/s2loop.h"
#include "s2/s2point.h"
#include "s2/s2point_vector_shape.h"
#include "s2/s2pointutil.h"
#include "s2/s2polygon.h"
#include "s2/s2polyline.h"
#include "s2/s2shape.h"
#include "s2/s2shape_index.h"
#include "third_party/core_math/core_math.h"

namespace googlesql_geography {
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

// External WKT: closed rings, with holes running clockwise.
template <typename Vertex>
std::string Text(const Geography& geography, Vertex vertex) {
  if (geography.Empty()) {
    return std::string(kEmpty);
  }
  if (geography.Full()) {
    return "GEOMETRYCOLLECTION(POLYGON((0 0, 120 0, -120 0, 0 0)), "
           "POLYGON((0 0, -120 0, 120 0, 0 0)))";
  }
  const auto vertices = [&](const std::vector<S2Point>& line) {
    return "(" + List(line, vertex) + ")";
  };
  const auto rings = [&](const Polygon& polygon) {
    std::vector<std::string> items;
    for (const std::vector<S2Point>& loop : polygon) {
      std::vector<S2Point> ring = loop;
      if (&loop != polygon.data()) {
        std::ranges::reverse(ring);
      }
      ring.push_back(ring.front());
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
  const double phi = S1Angle::Degrees(point.latitude).radians();
  const double theta = S1Angle::Degrees(point.longitude).radians();
  const double cos_phi = cr_cos(phi);
  return {cr_cos(theta) * cos_phi, cr_sin(theta) * cos_phi, cr_sin(phi)};
}

S2LatLng ToLatLng(const S2Point& point) {
  // As S2LatLng::Latitude and Longitude, whose "+ 0.0" turns -0 into 0.
  return S2LatLng::FromRadians(
      cr_atan2(point.z() + 0.0, std::sqrt((point.x() * point.x()) + (point.y() * point.y()))),
      cr_atan2(point.y() + 0.0, point.x() + 0.0));
}

Point ToPoint(const S2Point& point) {
  const S2LatLng latlng = ToLatLng(point);
  return {.longitude = latlng.lng().degrees(), .latitude = latlng.lat().degrees()};
}

R2Point DegreesProjection::Project(const S2Point& p) const {
  const Point point = ToPoint(p);
  return {point.longitude, point.latitude};
}

S2Point DegreesProjection::Unproject(const R2Point& p) const {
  return ToS2Point({.longitude = std::remainder(p.x(), 360.0), .latitude = p.y()});
}

Geography PointGeography(const S2Point& point) {
  return {.points = {point}, .lines = {}, .polygons = {}};
}

Geography FullGeography() {
  const auto vertices = S2Loop::kFull();
  return {.points = {}, .lines = {}, .polygons = {{{vertices.begin(), vertices.end()}}}};
}

std::string Wkt(const Geography& geography) {
  return Text(geography, [](const S2Point& vertex) {
    const Point point = ToPoint(vertex);
    return Coordinate(point.longitude) + " " + Coordinate(point.latitude);
  });
}

void AddShapes(const Geography& geography, MutableS2ShapeIndex& index) {
  for (int id = 0; id < ShapeCount(geography); ++id) {
    index.Add(Shape(geography, id));
  }
}

int ShapeCount(const Geography& geography) {
  return static_cast<int>(geography.lines.size()) + (geography.points.empty() ? 0 : 1) +
         (geography.polygons.empty() ? 0 : 1);
}

std::unique_ptr<S2Shape> Shape(const Geography& geography, int id) {
  const int lines = id - (geography.points.empty() ? 0 : 1);
  if (lines < 0) {
    return std::make_unique<S2PointVectorShape>(geography.points);
  }
  if (std::cmp_less(lines, geography.lines.size())) {
    return std::make_unique<S2LaxPolylineShape>(geography.lines.at(lines));
  }
  return std::make_unique<S2Polygon::OwningShape>(ToS2Polygon(geography));
}

IndexedGeography::IndexedGeography(Geography geography)
    : geography_(std::make_shared<const Geography>(std::move(geography))) {}

IndexedGeography::IndexedGeography(std::shared_ptr<const Geography> geography,
                                   std::shared_ptr<const S2ShapeIndex> index)
    : geography_(std::move(geography)), index_(std::move(index)) {}

const S2ShapeIndex& IndexedGeography::index() const {
  if (index_ == nullptr) {
    auto built = std::make_shared<MutableS2ShapeIndex>();
    AddShapes(*geography_, *built);
    index_ = std::move(built);
  }
  return *index_;
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
    if (vertices.empty()) {
      if (&vertices == input.data()) {
        rings.emplace_back();
        continue;
      }
      return absl::OutOfRangeError(std::string(failed) +
                                   "Polygon holes cannot be empty/full loops.");
    }
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
    if (ring.empty()) {
      continue;
    }
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

// The polygons of `polygon`, whose loops run in preorder: each shell is followed by its holes
// and what lies in them. S2 represents the full polygon with its sentinel vertex.
Geography FromPolygon(const S2Polygon& polygon) {
  Geography geography;
  std::vector<size_t> polygon_of(polygon.num_loops());
  for (int i = 0; i < polygon.num_loops(); ++i) {
    const S2Loop* loop = polygon.loop(i);
    if (loop->is_hole()) {
      polygon_of.at(i) = polygon_of.at(polygon.GetParent(i));
    } else {
      polygon_of.at(i) = geography.polygons.size();
      geography.polygons.emplace_back();
    }
    const auto vertices = loop->vertices_span();
    geography.polygons.at(polygon_of.at(i)).emplace_back(vertices.begin(), vertices.end());
  }
  return geography;
}

// The polygon of distinct rings, which must be valid: each encloses the smaller region or, where
// `oriented`, the one on its left, which a hole removes from the shell's. An empty first ring is a
// full shell.
absl::StatusOr<Geography> Assemble(const std::vector<std::vector<S2Point>>& rings,
                                   std::string_view failed, bool oriented) {
  GOOGLESQL_ASSIGN_OR_RETURN(std::vector<std::unique_ptr<S2Loop>> loops, Loops(rings, failed));
  std::vector<const S2Loop*> order;
  for (const std::unique_ptr<S2Loop>& loop : loops) {
    if (!oriented) {
      loop->Normalize();
    }
    order.push_back(loop.get());
  }
  S2Polygon polygon;
  polygon.set_s2debug_override(S2Debug::DISABLE);
  polygon.InitNested(std::move(loops));
  S2Error error;
  if (polygon.FindValidationError(&error)) {
    return absl::OutOfRangeError(std::string(failed) + (oriented ? "" : "Invalid polygon: ") +
                                 std::string(error.message()));
  }
  if (rings.front().empty()) {
    for (size_t i = 0; i < order.size(); ++i) {
      if (order.at(i)->depth() != 0) {
        return absl::OutOfRangeError(std::string(failed) +
                                     "Polygon should have only one shell, holes must be inside "
                                     "the shell. Polygon hole " +
                                     std::to_string(i + 1) + " is outside of the shell");
      }
    }
    polygon.Invert();
    return FromPolygon(polygon);
  }
  if (oriented && order.front()->depth() != 0) {
    for (int i = 0; i < polygon.num_loops(); ++i) {
      if (polygon.loop(i) == order.front()) {
        const S2Loop* parent = polygon.loop(polygon.GetParent(i));
        const auto position = std::ranges::find(order, parent);
        return absl::OutOfRangeError(std::string(failed) +
                                     "Polygon's first loop must be shell. It is nested in loop " +
                                     std::to_string(std::distance(order.begin(), position) + 1));
      }
    }
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

// Defined beside the closed-set output helpers below, so construction and set operations use
// the same treatment of degenerate shells and holes.
absl::StatusOr<Geography> SnapPolygon(const std::vector<std::vector<S2Point>>& rings,
                                      std::string_view failed, bool oriented);

}  // namespace

absl::StatusOr<Geography> BuildPolygon(const std::vector<std::vector<S2Point>>& input,
                                       std::string_view failed, bool oriented) {
  GOOGLESQL_ASSIGN_OR_RETURN(const std::vector<std::vector<S2Point>> rings,
                             DistinctRings(input, failed));
  // BigQuery checks and normalizes each ring before snapping, and checks how the rings lie after.
  GOOGLESQL_ASSIGN_OR_RETURN(const std::vector<std::unique_ptr<S2Loop>> loops,
                             Loops(rings, failed));
  std::vector<std::vector<S2Point>> normalized;
  if (rings.front().empty()) {
    normalized.emplace_back();
  }
  for (const std::unique_ptr<S2Loop>& loop : loops) {
    if (!oriented) {
      loop->Normalize();
    }
    const absl::Span<const S2Point> vertices = loop->vertices_span();
    normalized.emplace_back(vertices.begin(), vertices.end());
  }
  return SnapPolygon(normalized, failed, oriented);
}

namespace {

// The output of a boolean operation, which a subsequent union can read.
struct BooleanResult {
  std::vector<S2Point> points;
  std::vector<std::unique_ptr<S2Polyline>> lines;
  S2Polygon polygon;
};

// The geography of `output`, whose points on a linestring or a polygon's ring, which S2 keeps
// unless they are its vertices, are dropped as BigQuery drops them.
Geography Read(const BooleanResult& output) {
  Geography geography = FromPolygon(output.polygon);
  absl::flat_hash_set<S2Point> vertices;
  for (const std::unique_ptr<S2Polyline>& polyline : output.lines) {
    const absl::Span<const S2Point> line = polyline->vertices_span();
    geography.lines.emplace_back(line.begin(), line.end());
    vertices.insert(line.begin(), line.end());
  }
  for (int i = 0; i < output.polygon.num_loops(); ++i) {
    const S2Loop* loop = output.polygon.loop(i);
    const absl::Span<const S2Point> ring = loop->vertices_span();
    vertices.insert(ring.begin(), ring.end());
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
  auto output = std::make_unique<BooleanResult>();
  auto index = std::make_unique<MutableS2ShapeIndex>();
  for (const std::unique_ptr<MutableS2ShapeIndex>& input : inputs) {
    auto next = std::make_unique<BooleanResult>();
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

namespace {

absl::StatusOr<Geography> SnapPolygon(const std::vector<std::vector<S2Point>>& rings,
                                      std::string_view failed, bool oriented) {
  S2Builder::Options options(s2builderutil::IdentitySnapFunction(S2Earth::MetersToAngle(1e-6)));
  options.set_split_crossing_edges(true);
  S2Builder builder(options);
  // Snap each ring in the same builder as the result. Checking their interiors afterwards
  // permits shared boundaries, but not holes outside the shell or overlapping holes.
  std::vector<S2Polygon> interiors(rings.size());
  s2builderutil::S2PolygonLayer::Options polygon_options;
  polygon_options.set_validate(true);
  for (size_t i = 0; i < rings.size(); ++i) {
    builder.StartLayer(
        std::make_unique<s2builderutil::S2PolygonLayer>(&interiors.at(i), polygon_options));
    builder.AddIsFullPolygonPredicate(S2Builder::IsFullPolygon(rings.at(i).empty()));
    builder.AddLoop(rings.at(i));
  }
  BooleanResult output;
  using LineLayer = s2builderutil::S2PolylineVectorLayer;
  LineLayer::Options line_options;
  line_options.set_edge_type(S2Builder::EdgeType::UNDIRECTED);
  line_options.set_polyline_type(S2Builder::Graph::PolylineType::WALK);
  line_options.set_duplicate_edges(LineLayer::Options::DuplicateEdges::MERGE);
  std::vector<std::unique_ptr<S2Builder::Layer>> layers;
  layers.push_back(std::make_unique<s2builderutil::S2PointVectorLayer>(&output.points));
  layers.push_back(std::make_unique<LineLayer>(&output.lines, line_options));
  layers.push_back(
      std::make_unique<s2builderutil::S2PolygonLayer>(&output.polygon, polygon_options));
  auto normalized = s2builderutil::NormalizeClosedSet(std::move(layers));
  builder.StartLayer(std::move(normalized.at(0)));
  builder.StartLayer(std::move(normalized.at(1)));
  builder.StartLayer(std::move(normalized.at(2)));
  builder.AddIsFullPolygonPredicate(S2Builder::IsFullPolygon(rings.front().empty()));
  for (size_t i = 0; i < rings.size(); ++i) {
    auto ring = rings.at(i);
    if (i != 0) {
      std::ranges::reverse(ring);
    }
    builder.AddLoop(ring);
  }
  S2Error error;
  if (!builder.Build(&error)) {
    const auto original = Assemble(rings, failed, oriented);
    if (!original.ok()) {
      return original.status();
    }
    return absl::OutOfRangeError(std::string(failed) +
                                 "Invalid polygon: " + std::string(error.message()));
  }
  // Shared vertices are allowed, but a shared edge between nondegenerate rings is not.
  // Use the snapped interiors, so collapsed shells and holes have no boundary to reject.
  absl::flat_hash_set<std::pair<S2Point, S2Point>> edges;
  for (const S2Polygon& interior : interiors) {
    for (int i = 0; i < interior.num_loops(); ++i) {
      const S2Loop& loop = *interior.loop(i);
      for (int j = 0; j < loop.num_vertices(); ++j) {
        const S2Point& a = loop.vertex(j);
        const S2Point& b = loop.vertex(j + 1);
        if (a != b && !edges.emplace(std::min(a, b), std::max(a, b)).second) {
          return absl::OutOfRangeError(std::string(failed) +
                                       "Shape 0 has one or more edges with interior on the right.");
        }
      }
    }
  }
  for (size_t i = 1; i < interiors.size(); ++i) {
    bool valid = interiors.front().Contains(interiors.at(i));
    for (size_t j = 1; j < i; ++j) {
      valid = valid && !interiors.at(j).Intersects(interiors.at(i));
    }
    if (!valid) {
      const auto original = Assemble(rings, failed, oriented);
      if (!original.ok()) {
        return original.status();
      }
      return absl::OutOfRangeError(std::string(failed) +
                                   "Invalid polygon: the holes must lie inside the shell and "
                                   "outside each other");
    }
  }
  return Read(output);
}

}  // namespace

absl::StatusOr<Geography> Snap(const Geography& geography,
                               const S2Builder::SnapFunction& snap_function, bool simplify) {
  S2Builder::Options options(snap_function);
  options.set_simplify_edge_chains(simplify);
  S2Builder builder(options);
  using PolylineLayer = s2builderutil::S2PolylineVectorLayer;
  PolylineLayer::Options line_options;
  line_options.set_polyline_type(S2Builder::Graph::PolylineType::WALK);
  line_options.set_edge_type(S2Builder::EdgeType::UNDIRECTED);
  line_options.set_duplicate_edges(PolylineLayer::Options::DuplicateEdges::MERGE);
  BooleanResult output;
  std::vector<std::unique_ptr<S2Builder::Layer>> layers;
  layers.push_back(std::make_unique<s2builderutil::S2PointVectorLayer>(&output.points));
  layers.push_back(std::make_unique<PolylineLayer>(&output.lines, line_options));
  layers.push_back(std::make_unique<s2builderutil::S2PolygonLayer>(&output.polygon));
  std::vector<std::unique_ptr<S2Builder::Layer>> normalized =
      s2builderutil::NormalizeClosedSet(std::move(layers));
  builder.StartLayer(std::move(normalized.at(0)));
  for (const S2Point& point : geography.points) {
    builder.AddPoint(point);
  }
  builder.StartLayer(std::move(normalized.at(1)));
  for (const std::vector<S2Point>& line : geography.lines) {
    builder.AddPolyline(line);
  }
  builder.StartLayer(std::move(normalized.at(2)));
  builder.AddIsFullPolygonPredicate(S2Builder::IsFullPolygon(geography.Full()));
  if (!geography.polygons.empty()) {
    builder.AddPolygon(*ToS2Polygon(geography));
  }
  S2Error error;
  if (!builder.Build(&error)) {
    return absl::InternalError("S2Builder failed: " + std::string(error.message()));
  }
  return Read(output);
}

absl::StatusOr<Geography> RepairPolygon(const std::vector<std::vector<S2Point>>& input,
                                        std::string_view failed) {
  S2Builder::Options options(s2builderutil::IdentitySnapFunction(S2Earth::MetersToAngle(1e-6)));
  options.set_split_crossing_edges(true);
  S2Builder builder(options);
  // Repair discards degenerate input edges rather than preserving them as lines or points.
  S2Polygon polygon;
  s2builderutil::S2PolygonLayer::Options polygon_options(S2Builder::EdgeType::UNDIRECTED);
  polygon_options.set_validate(true);
  builder.StartLayer(std::make_unique<s2builderutil::S2PolygonLayer>(&polygon, polygon_options));
  for (const auto& ring : input) {
    builder.AddLoop(ring);
  }
  S2Error error;
  if (!builder.Build(&error)) {
    return absl::OutOfRangeError(std::string(failed) + std::string(error.message()));
  }
  // Canonicalize touching holes into separate shells where the repaired interior has islands.
  MutableS2ShapeIndex polygon_index;
  const MutableS2ShapeIndex empty_index;
  AddShapes(FromPolygon(polygon), polygon_index);
  return ClosedSetOperation(polygon_index, empty_index, S2BooleanOperation::OpType::UNION);
}

absl::StatusOr<Geography> BufferGeography(const Geography& geography,
                                          const S2BufferOperation::Options& options) {
  S2Polygon polygon;
  S2BufferOperation operation(std::make_unique<s2builderutil::S2PolygonLayer>(&polygon), options);
  MutableS2ShapeIndex index;
  AddShapes(geography, index);
  operation.AddShapeIndex(index);
  S2Error error;
  if (!operation.Build(&error)) {
    return absl::InternalError("S2BufferOperation failed: " + std::string(error.message()));
  }
  return FromPolygon(polygon);
}

absl::StatusOr<Geography> ClosedSetOperation(const MutableS2ShapeIndex& a,
                                             const MutableS2ShapeIndex& b,
                                             S2BooleanOperation::OpType operation) {
  S2BooleanOperation::Options options;
  options.set_polygon_model(S2BooleanOperation::PolygonModel::CLOSED);
  options.set_polyline_model(S2BooleanOperation::PolylineModel::CLOSED);
  options.set_polyline_loops_have_boundaries(false);
  using PolylineLayer = s2builderutil::S2PolylineVectorLayer;
  PolylineLayer::Options line_options;
  line_options.set_polyline_type(S2Builder::Graph::PolylineType::WALK);
  line_options.set_edge_type(S2Builder::EdgeType::UNDIRECTED);
  line_options.set_duplicate_edges(PolylineLayer::Options::DuplicateEdges::MERGE);
  BooleanResult output;
  std::vector<std::unique_ptr<S2Builder::Layer>> layers;
  layers.push_back(std::make_unique<s2builderutil::S2PointVectorLayer>(&output.points));
  layers.push_back(std::make_unique<PolylineLayer>(&output.lines, line_options));
  layers.push_back(std::make_unique<s2builderutil::S2PolygonLayer>(&output.polygon));
  S2BooleanOperation op(operation, s2builderutil::NormalizeClosedSet(std::move(layers)), options);
  S2Error error;
  if (!op.Build(a, b, &error)) {
    return absl::InternalError("S2BooleanOperation failed: " + std::string(error.message()));
  }
  return Read(output);
}

}  // namespace googlesql_geography
