#include "src/backend_functions/geography.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <numbers>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/types/span.h"
#include "duckdb.h"
#include "googlesql/base/status_macros.h"
#include "s2/mutable_s2shape_index.h"
#include "s2/s1angle.h"
#include "s2/s1chord_angle.h"
#include "s2/s2builder.h"
#include "s2/s2builderutil_s2point_vector_layer.h"
#include "s2/s2builderutil_s2polyline_vector_layer.h"
#include "s2/s2builderutil_snap_functions.h"
#include "s2/s2cell_id.h"
#include "s2/s2closest_edge_query.h"
#include "s2/s2earth.h"
#include "s2/s2error.h"
#include "s2/s2furthest_edge_query.h"
#include "s2/s2hausdorff_distance_query.h"
#include "s2/s2latlng.h"
#include "s2/s2lax_polyline_shape.h"
#include "s2/s2point.h"
#include "s2/s2point_vector_shape.h"
#include "s2/s2pointutil.h"
#include "s2/s2polyline.h"
#include "s2/s2shape.h"
#include "s2/s2shape_measures.h"
#include "src/backend_functions/register.h"
#include "src/backend_functions/scalar.h"
#include "src/duckdb_handle.h"

namespace bigquery_emulator_duckdb::backend_functions {
namespace {

// A GEOGRAPHY holds S2Points, as BigQuery's do: POINT(x y z) or LINESTRING(x y z, ...) with the
// shortest digits that round-trip, or GEOMETRYCOLLECTION EMPTY. Coordinates are those of
// S2LatLng, which gives ST_X(ST_GEOGPOINT(0.1 + 0.2, 1)) 0.3000000000000001 as BigQuery does. The
// translator turns a GEOGRAPHY into WKT with bq_st_astext wherever a query returns or stores one.
struct Point {
  double longitude;
  double latitude;
};

constexpr std::string_view kEmpty = "GEOMETRYCOLLECTION EMPTY";

// The vertices of a geography: none for the empty geography, which has no geometry type, one for
// a point and more for a linestring. EachRow handles SQL NULLs.
using Geography = std::vector<S2Point>;

// The shortest digits that round-trip, or with `precision` 15 significant digits, as printf's
// %.15g writes them and BigQuery writes 0.1 + 0.2 as 0.3 and 0.0000001 as 1e-07. BigQuery writes
// -0 as 0.
std::string Coordinate(double value, std::optional<int> precision = 15) {
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

// `geography` as WKT with the vertices `vertex` writes.
template <typename Vertex>
std::string Text(const Geography& geography, Vertex vertex) {
  if (geography.empty()) {
    return std::string(kEmpty);
  }
  std::string text = geography.size() == 1 ? "POINT(" : "LINESTRING(";
  for (const S2Point& point : geography) {
    text += (&point == geography.data() ? "" : ", ") + vertex(point);
  }
  return text + ")";
}

// The WKT BigQuery returns for `geography`, whose coordinates have 15 significant digits.
std::string Wkt(const Geography& geography) {
  return Text(geography, [](const S2Point& vertex) {
    const Point point = ToPoint(vertex);
    return Coordinate(point.longitude) + " " + Coordinate(point.latitude);
  });
}

// The GEOGRAPHY of `geography`.
std::string GeographyText(const Geography& geography) {
  return Text(geography, [](const S2Point& vertex) {
    return Coordinate(vertex.x(), std::nullopt) + " " + Coordinate(vertex.y(), std::nullopt) + " " +
           Coordinate(vertex.z(), std::nullopt);
  });
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

// Parses a GEOGRAPHY as GeographyText writes it.
absl::StatusOr<Geography> ParseGeography(std::string_view text) {
  if (text == kEmpty) {
    return Geography{};
  }
  for (const std::string_view prefix : {"POINT(", "LINESTRING("}) {
    if (!text.starts_with(prefix) || !text.ends_with(')')) {
      continue;
    }
    std::string_view vertices = text.substr(prefix.size(), text.size() - prefix.size() - 1);
    Geography geography;
    while (true) {
      const size_t comma = vertices.find(", ");
      const std::optional<S2Point> vertex = ReadVertex(vertices.substr(0, comma));
      if (!vertex) {
        break;
      }
      geography.push_back(*vertex);
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

// The point of a single-point geography, or else an error that `context` starts and that says
// what the geography is, unless `say` is false.
absl::StatusOr<S2Point> ParsePoint(const Arguments& args, idx_t index, std::string_view context,
                                   bool say = true) {
  GOOGLESQL_ASSIGN_OR_RETURN(const Geography geography, ParseGeography(args.String(index)));
  if (geography.size() != 1) {
    return absl::OutOfRangeError(std::string(context) + " must be a single point geography" +
                                 (!say                ? "."
                                  : geography.empty() ? "; an argument was the empty geography."
                                                      : "; an argument contained a line."));
  }
  return geography.front();
}

// The shape of a nonempty geography.
std::unique_ptr<S2Shape> Shape(const Geography& geography) {
  if (geography.size() == 1) {
    return std::make_unique<S2PointVectorShape>(geography);
  }
  return std::make_unique<S2LaxPolylineShape>(geography);
}

// The same point as BigQuery's ST_EQUALS and ST_AZIMUTH take it, within a micrometer in every
// direction: POINT(0.30000000000000004 0) is POINT(0.3 0), though their distance is not 0, and a
// pole is the same at every longitude.
bool Same(const S2Point& a, const S2Point& b) {
  return S2::ApproxEquals(a, b, S2Earth::MetersToAngle(1e-6));
}

// BigQuery takes points as antipodal only where their S2Points are exactly opposite, as
// POINT(-30 0) and POINT(150 0) are and POINT(10 20) and POINT(-170 -20) are not. Compared by
// component: S2Point's unary minus trips clang-tidy's analyzer inside S2.
bool Antipodal(const S2Point& p, const S2Point& q) {
  return p.x() == -q.x() && p.y() == -q.y() && p.z() == -q.z();
}

// "point" or "points" after `count`, as BigQuery's errors write it.
std::string Points(size_t count) {
  return std::to_string(count) + (count == 1 ? " point" : " points");
}

// Fails where consecutive `vertices` are antipodal, so that their edge has no direction: their
// S2Points are opposite, or their coordinates are, as BigQuery takes LINESTRING(0 0, 180 0) and
// the poles, whose S2Points are not. `failed` prefixes the error.
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

// BigQuery adds the midpoint of an edge as long as LINESTRING(-170 10, 0 0), about 165.894
// degrees, and not of one of 100 degrees; between them, it is not known where it starts.
constexpr double kSplitDegrees = 165.89;
constexpr double kUnsplitDegrees = 100;

// The linestring of `vertices`, two or more without antipodal neighbors, as BigQuery builds it
// with S2Builder: vertices within a micrometer are snapped together, a line folding back on itself
// merges with itself, and crossing edges are split where they cross. A long edge gains its
// midpoint, and a line that snaps to one point is the point S2Builder snaps it to. The emulator
// does not hold the MULTILINESTRING a line that overlaps itself becomes.
absl::StatusOr<Geography> BuildLine(const Geography& vertices) {
  Geography split;
  for (size_t i = 0; i < vertices.size(); ++i) {
    if (i > 0) {
      const S2Point& a = vertices.at(i - 1);
      const S2Point& b = vertices.at(i);
      const double degrees = S1Angle(a, b).degrees();
      if (degrees > kUnsplitDegrees && degrees < kSplitDegrees) {
        return absl::UnimplementedError(
            "The emulator does not support a LINESTRING edge longer than 100 degrees and shorter "
            "than 165.89");
      }
      if (degrees >= kSplitDegrees) {
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
    return Geography{sites.front()};
  }
  if (polylines.size() > 1) {
    return absl::UnimplementedError(
        "The emulator does not support a LINESTRING that BigQuery splits into a "
        "MULTILINESTRING");
  }
  const absl::Span<const S2Point> line = polylines.front()->vertices_span();
  return Geography(line.begin(), line.end());
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
  return GeographyText(
      {ToS2Point({.longitude = std::remainder(longitude, 360.0), .latitude = latitude})});
}

// Reads the WKT of a point or a linestring as ST_GEOGFROMTEXT does, with any case and whitespace,
// failing as BigQuery does with the 0-based position of what it saw instead of what it expected.
class WktReader {
 public:
  explicit WktReader(std::string_view text) : text_(text) {}

  // Whether the next token is `word`, ignoring case, which it then skips.
  bool Word(std::string_view word) {
    Spaces();
    const std::string_view token = Token();
    if (token.size() != word.size() || !std::ranges::equal(token, word, [](char a, char b) {
          return std::toupper(static_cast<unsigned char>(a)) == b;
        })) {
      return false;
    }
    position_ += token.size();
    return true;
  }

  // Skips `what`, or fails saying `expected`, by default '`what`', was expected.
  absl::Status Expect(std::string_view what, std::optional<std::string_view> expected = {}) {
    if (Word(what)) {
      return absl::OkStatus();
    }
    return Error(expected ? std::string(*expected) : "'" + std::string(what) + "'");
  }

  absl::StatusOr<double> Number(std::string_view what) {
    Spaces();
    const std::string_view token = Token();
    double value = 0;
    const auto result = std::from_chars(token.data(), token.data() + token.size(), value);
    if (token.empty() || result.ec != std::errc() || result.ptr != token.data() + token.size() ||
        !std::ranges::all_of(token, [](char c) {
          return std::isdigit(static_cast<unsigned char>(c)) != 0 || c == '.' || c == '-' ||
                 c == '+' || c == 'e' || c == 'E';
        })) {
      return Error(std::string(what));
    }
    position_ += token.size();
    return value;
  }

  absl::Status End() {
    Spaces();
    if (position_ == text_.size()) {
      return absl::OkStatus();
    }
    return Error("end of data");
  }

 private:
  void Spaces() {
    while (position_ < text_.size() &&
           std::isspace(static_cast<unsigned char>(text_.at(position_))) != 0) {
      ++position_;
    }
  }

  // A run of letters, digits and number signs, or else one character.
  [[nodiscard]] std::string_view Token() const {
    const auto word = [](char c) {
      return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '.' || c == '-' || c == '+';
    };
    size_t end = position_;
    while (end < text_.size() && word(text_.at(end))) {
      ++end;
    }
    if (end == position_ && end < text_.size()) {
      ++end;
    }
    return text_.substr(position_, end - position_);
  }

  [[nodiscard]] absl::Status Error(const std::string& expected) const {
    const std::string_view token = Token();
    return absl::OutOfRangeError(
        "ST_GeogFromText failed: Expected " + expected + ", but saw " +
        (token.empty() ? std::string("end of data") : "'" + std::string(token) + "'") +
        " at position " + std::to_string(position_) + ".");
  }

  std::string_view text_;
  size_t position_ = 0;
};

// Points and linestrings, whose longitudes are taken modulo 360 as ST_GEOGPOINT takes them, and
// empty geometries.
absl::StatusOr<std::string> GeogFromText(const Arguments& args) {
  const std::string text = args.String(0);
  WktReader reader(text);
  const bool point = reader.Word("POINT");
  const bool line = !point && reader.Word("LINESTRING");
  const bool geometry = point || line || reader.Word("POLYGON") || reader.Word("MULTIPOINT") ||
                        reader.Word("MULTILINESTRING") || reader.Word("MULTIPOLYGON") ||
                        reader.Word("GEOMETRYCOLLECTION");
  if (geometry && reader.Word("EMPTY")) {
    GOOGLESQL_RETURN_IF_ERROR(reader.End());
    return std::string(kEmpty);
  }
  if (!point && !line) {
    return absl::UnimplementedError("The emulator does not support ST_GEOGFROMTEXT of " + text);
  }
  GOOGLESQL_RETURN_IF_ERROR(reader.Expect("(", "'(' or EMPTY"));
  std::vector<Point> geography;
  while (true) {
    GOOGLESQL_ASSIGN_OR_RETURN(const double longitude, reader.Number("longitude"));
    GOOGLESQL_ASSIGN_OR_RETURN(const double latitude, reader.Number("latitude"));
    geography.push_back({.longitude = longitude, .latitude = latitude});
    if (!line || !reader.Word(",")) {
      break;
    }
  }
  GOOGLESQL_RETURN_IF_ERROR(reader.Expect(")", line ? "',' or ')'" : "')'"));
  GOOGLESQL_RETURN_IF_ERROR(reader.End());
  for (Point& vertex : geography) {
    if (std::abs(vertex.latitude) > 90) {
      return absl::OutOfRangeError(
          "ST_GeogFromText failed: Latitude must be between -90 and 90 degrees. Actual value "
          "was " +
          Coordinate(vertex.latitude));
    }
    vertex.longitude = std::remainder(vertex.longitude, 360.0);
  }
  Geography vertices(geography.size());
  std::ranges::transform(geography, vertices.begin(), ToS2Point);
  if (!line) {
    return GeographyText(vertices);
  }
  if (geography.size() < 2) {
    return absl::OutOfRangeError(
        "ST_GeogFromText failed: Linestring should have at least two points, but had " +
        Points(geography.size()));
  }
  GOOGLESQL_RETURN_IF_ERROR(CheckAntipodal(geography, "ST_GeogFromText failed: "));
  GOOGLESQL_ASSIGN_OR_RETURN(const Geography built, BuildLine(vertices));
  return GeographyText(built);
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
  if (a.empty() || b.empty()) {
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
  if (a.empty() || b.empty()) {
    return false;
  }
  const double limit = args.Double(2);
  if (a.size() == 1 && b.size() == 1) {
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
// takes points. A point differs from a linestring, which has more than one point.
absl::StatusOr<bool> Equals(const Arguments& args) {
  GOOGLESQL_ASSIGN_OR_RETURN(const Geography a, ParseGeography(args.String(0)));
  GOOGLESQL_ASSIGN_OR_RETURN(const Geography b, ParseGeography(args.String(1)));
  if (a.size() < 2 || b.size() < 2) {
    return a.size() == b.size() && (a.empty() || Same(a.front(), b.front()));
  }
  MutableS2ShapeIndex index;
  index.Add(Shape(a));
  MutableS2ShapeIndex other;
  other.Add(Shape(b));
  return S2HausdorffDistanceQuery().GetDistance(&index, &other) <=
         S1ChordAngle(S2Earth::MetersToAngle(1e-6));
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

// The dimension of the empty geography, a point and a linestring.
int64_t Dimension(const Geography& geography) {
  return std::min<int64_t>(static_cast<int64_t>(geography.size()), 2) - 1;
}

std::string GeometryType(const Geography& geography) {
  constexpr std::array<std::string_view, 3> kTypes = {
      "ST_GeometryCollection",
      "ST_Point",
      "ST_LineString",
  };
  return std::string(kTypes.at(Dimension(geography) + 1));
}

// A point is closed, and a linestring where it ends where it starts.
bool IsClosed(const Geography& geography) {
  return !geography.empty() && Same(geography.front(), geography.back());
}

// ST_LENGTH, the length of a linestring's edges, which is 0 for a point or an empty geography.
absl::StatusOr<double> Length(const Arguments& args) {
  GOOGLESQL_RETURN_IF_ERROR(CheckSphere(args, 1));
  GOOGLESQL_ASSIGN_OR_RETURN(const Geography geography, ParseGeography(args.String(0)));
  return geography.size() < 2 ? 0.0 : S2Earth::ToMeters(S2::GetLength(*Shape(geography)));
}

// ST_AREA and ST_PERIMETER, which are 0 for anything but a polygon.
absl::StatusOr<double> ZeroMeasure(const Arguments& args) {
  GOOGLESQL_RETURN_IF_ERROR(CheckSphere(args, 1));
  GOOGLESQL_RETURN_IF_ERROR(ParseGeography(args.String(0)).status());
  return 0.0;
}

// BigQuery adds vertices to the GeoJSON of a linestring to keep its planar edges near the
// geodesic ones, which the emulator does not.
absl::StatusOr<std::string> AsGeoJson(const Arguments& args) {
  GOOGLESQL_ASSIGN_OR_RETURN(const Geography geography, ParseGeography(args.String(0)));
  if (geography.empty()) {
    return std::string(R"({ "type": "GeometryCollection", "geometries": [ ] } )");
  }
  if (geography.size() > 1) {
    return absl::UnimplementedError("The emulator does not support ST_ASGEOJSON of a LINESTRING");
  }
  const Point point = ToPoint(geography.front());
  return R"({ "type": "Point", "coordinates": [)" + Coordinate(point.longitude) + ", " +
         Coordinate(point.latitude) + "] } ";
}

// ST_STARTPOINT, ST_ENDPOINT and ST_POINTN: the vertex of a linestring at a 1-based index, a
// negative one counting from the end.
absl::StatusOr<std::string> PointN(const Arguments& args) {
  GOOGLESQL_ASSIGN_OR_RETURN(const Geography line, ParseGeography(args.String(0)));
  const int64_t index = args.Int(1);
  const auto size = static_cast<int64_t>(line.size());
  if (size < 2) {
    return absl::OutOfRangeError("The input is not a single LINESTRING");
  }
  if (index == 0 || index > size || index < -size) {
    return absl::OutOfRangeError("The vertex index " + std::to_string(index) +
                                 " is out of bounds; input LINESTRING has " + Points(line.size()) +
                                 ".");
  }
  return GeographyText({line.at(index > 0 ? index - 1 : size + index)});
}

// The line through the vertices of the points and linestrings of `geographies`, built as
// ST_GEOGFROMTEXT builds one; a single point is that point.
absl::StatusOr<std::string> MakeLine(const std::vector<std::string>& geographies) {
  Geography vertices;
  for (const std::string& text : geographies) {
    GOOGLESQL_ASSIGN_OR_RETURN(const Geography geography, ParseGeography(text));
    if (geography.empty()) {
      return absl::OutOfRangeError("ST_MAKELINE arguments must be single points or lines");
    }
    vertices.insert(vertices.end(), geography.begin(), geography.end());
  }
  if (vertices.empty()) {
    return absl::OutOfRangeError(
        "ST_MAKELINE failed: Linestring should have at least two points, but had " +
        Points(vertices.size()));
  }
  if (vertices.size() == 1) {
    return GeographyText(vertices);
  }
  std::vector<Point> points(vertices.size());
  std::ranges::transform(vertices, points.begin(), ToPoint);
  GOOGLESQL_RETURN_IF_ERROR(CheckAntipodal(points, ""));
  GOOGLESQL_ASSIGN_OR_RETURN(const Geography line, BuildLine(vertices));
  return GeographyText(line);
}

constexpr std::string_view kGeohashDigits = "0123456789bcdefghjkmnpqrstuvwxyz";

// A maxchars outside [1, 20] is taken as 20.
absl::StatusOr<std::optional<std::string>> Geohash(const Arguments& args) {
  GOOGLESQL_ASSIGN_OR_RETURN(const Geography geography, ParseGeography(args.String(0)));
  if (geography.empty()) {
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
  return GeographyText({ToS2Point({.longitude = middle(0), .latitude = middle(1)})});
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
  Register(connection, "bq_st_x", {kVarchar}, kDouble, EachRow<X>);
  Register(connection, "bq_st_y", {kVarchar}, kDouble, EachRow<Y>);
  Register(connection, "bq_st_geometrytype", {kVarchar}, kVarchar,
           EachRow<OfGeography<GeometryType>>);
  Register(connection, "bq_st_dimension", {kVarchar}, kBigint, EachRow<OfGeography<Dimension>>);
  Register(connection, "bq_st_numpoints", {kVarchar}, kBigint,
           EachRow<OfGeography<[](const Geography& geography) {
             return static_cast<int64_t>(geography.size());
           }>>);
  Register(connection, "bq_st_isempty", {kVarchar}, kBoolean,
           EachRow<OfGeography<[](const Geography& geography) { return geography.empty(); }>>);
  // A point or a linestring is never a collection.
  Register(connection, "bq_st_iscollection", {kVarchar}, kBoolean,
           EachRow<OfGeography<[](const Geography&) { return false; }>>);
  Register(connection, "bq_st_isclosed", {kVarchar}, kBoolean, EachRow<OfGeography<IsClosed>>);
  Register(connection, "bq_st_length", {kVarchar, kBoolean, kVarchar}, kDouble, EachRow<Length>);
  Register(connection, "bq_st_zero_measure", {kVarchar, kBoolean, kVarchar}, kDouble,
           EachRow<ZeroMeasure>);
  Register(connection, "bq_st_pointn", {kVarchar, kBigint}, kVarchar, EachRow<PointN>);
  Register(
      connection, "bq_st_makeline", {kVarchar, kVarchar}, kVarchar,
      EachRow<[](const Arguments& args) { return MakeLine({args.String(0), args.String(1)}); }>);
  {
    const LogicalType varchar(duckdb_create_logical_type(kVarchar));
    const LogicalType list(duckdb_create_list_type(varchar.get()));
    Register(connection, "bq_st_makeline_array", {list.get()}, varchar.get(),
             EachRow<[](const Arguments& args) { return MakeLine(args.Strings(0)); }>);
  }
  Register(connection, "bq_st_geohash", {kVarchar, kBigint}, kVarchar, EachRow<Geohash>);
  Register(connection, "bq_s2_cellidfrompoint", {kVarchar, kBigint}, kBigint,
           EachRow<CellIdFromPoint>);
}

}  // namespace bigquery_emulator_duckdb::backend_functions
