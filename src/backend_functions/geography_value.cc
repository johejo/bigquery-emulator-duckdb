#include "src/backend_functions/geography_value.h"

#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/types/span.h"
#include "s2/s1angle.h"
#include "s2/s2builder.h"
#include "s2/s2builderutil_s2point_vector_layer.h"
#include "s2/s2builderutil_s2polyline_vector_layer.h"
#include "s2/s2builderutil_snap_functions.h"
#include "s2/s2earth.h"
#include "s2/s2error.h"
#include "s2/s2latlng.h"
#include "s2/s2lax_polyline_shape.h"
#include "s2/s2point.h"
#include "s2/s2point_vector_shape.h"
#include "s2/s2pointutil.h"
#include "s2/s2polyline.h"
#include "s2/s2shape.h"

namespace bigquery_emulator_duckdb::backend_functions {
namespace {

constexpr std::string_view kEmpty = "GEOMETRYCOLLECTION EMPTY";

// `geography` as WKT with the vertices `vertex` writes.
template <typename Vertex>
std::string Text(const Geography& geography, Vertex vertex) {
  std::string text;
  switch (geography.kind) {
    case Kind::kEmpty:
      return std::string(kEmpty);
    case Kind::kPoint:
      text = "POINT(";
      break;
    case Kind::kLineString:
      text = "LINESTRING(";
      break;
  }
  for (const S2Point& point : geography.vertices) {
    text += (&point == geography.vertices.data() ? "" : ", ") + vertex(point);
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
  return {.kind = Kind::kPoint, .vertices = {point}};
}

std::string Wkt(const Geography& geography) {
  return Text(geography, [](const S2Point& vertex) {
    const Point point = ToPoint(vertex);
    return Coordinate(point.longitude) + " " + Coordinate(point.latitude);
  });
}

std::string GeographyText(const Geography& geography) {
  return Text(geography, [](const S2Point& vertex) {
    return Coordinate(vertex.x(), std::nullopt) + " " + Coordinate(vertex.y(), std::nullopt) + " " +
           Coordinate(vertex.z(), std::nullopt);
  });
}

absl::StatusOr<Geography> ParseGeography(std::string_view text) {
  if (text == kEmpty) {
    return Geography{};
  }
  for (const auto& [prefix, kind] : {
           std::pair<std::string_view, Kind>{"POINT(", Kind::kPoint},
           {"LINESTRING(", Kind::kLineString},
       }) {
    if (!text.starts_with(prefix) || !text.ends_with(')')) {
      continue;
    }
    std::string_view vertices = text.substr(prefix.size(), text.size() - prefix.size() - 1);
    Geography geography{.kind = kind, .vertices = {}};
    while (true) {
      const size_t comma = vertices.find(", ");
      const std::optional<S2Point> vertex = ReadVertex(vertices.substr(0, comma));
      if (!vertex) {
        break;
      }
      geography.vertices.push_back(*vertex);
      if (comma == std::string_view::npos) {
        return geography;
      }
      vertices.remove_prefix(comma + 2);
    }
  }
  return absl::UnimplementedError(
      "The emulator does not support GEOGRAPHY values other than points, linestrings or empty "
      "geometries: " +
      std::string(text));
}

std::unique_ptr<S2Shape> Shape(const Geography& geography) {
  if (geography.kind == Kind::kPoint) {
    return std::make_unique<S2PointVectorShape>(geography.vertices);
  }
  return std::make_unique<S2LaxPolylineShape>(geography.vertices);
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
  return Geography{.kind = Kind::kLineString, .vertices = {line.begin(), line.end()}};
}

}  // namespace bigquery_emulator_duckdb::backend_functions
