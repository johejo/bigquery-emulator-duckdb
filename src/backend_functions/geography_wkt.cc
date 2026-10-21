#include "src/backend_functions/geography_wkt.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "googlesql/base/status_macros.h"
#include "s2/s2loop_measures.h"
#include "s2/s2point.h"
#include "src/backend_functions/geography_value.h"

namespace bigquery_emulator_duckdb::backend_functions {
namespace {

// Reads the WKT of a point, a linestring or a polygon as ST_GEOGFROMTEXT does, with any case and
// whitespace, failing as BigQuery does with the 0-based position of what it saw instead of what it
// expected.
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

  // Fails saying the next token, a number or else one character, was unexpected.
  [[nodiscard]] absl::Status Unexpected() {
    Spaces();
    size_t end = position_;
    while (end < text_.size() &&
           (std::isdigit(static_cast<unsigned char>(text_.at(end))) != 0 || text_.at(end) == '.' ||
            text_.at(end) == '-' || text_.at(end) == '+')) {
      ++end;
    }
    if (end == position_ && end < text_.size()) {
      ++end;
    }
    return absl::InvalidArgumentError(
        "ST_GeogFromText failed: Unexpected " +
        (end == position_ ? std::string("end of data")
                          : "'" + std::string(text_.substr(position_, end - position_)) + "'") +
        " at position " + std::to_string(position_) + ".");
  }

  // Whether the next character starts a word.
  bool Letter() {
    Spaces();
    return position_ < text_.size() &&
           std::isalpha(static_cast<unsigned char>(text_.at(position_))) != 0;
  }

  [[nodiscard]] std::string_view Text() const { return text_; }

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

// Reads `(x y, ...)`, which `open` says was expected where it does not start, of one vertex
// unless `many`.
absl::StatusOr<std::vector<Point>> ReadVertices(WktReader& reader, std::string_view open,
                                                bool many) {
  GOOGLESQL_RETURN_IF_ERROR(reader.Expect("(", open));
  std::vector<Point> points;
  while (true) {
    GOOGLESQL_ASSIGN_OR_RETURN(const double longitude, reader.Number("longitude"));
    GOOGLESQL_ASSIGN_OR_RETURN(const double latitude, reader.Number("latitude"));
    points.push_back({.longitude = longitude, .latitude = latitude});
    if (!many || !reader.Word(",")) {
      break;
    }
  }
  GOOGLESQL_RETURN_IF_ERROR(reader.Expect(")", many ? "',' or ')'" : "')'"));
  return points;
}

// A point, a linestring or a polygon as WKT gives it, before it is built.
struct Part {
  enum class Type : uint8_t { kPoint, kLineString, kPolygon, kFull };
  Type type;
  // A polygon's rings, or the vertices of a point or a linestring.
  std::vector<std::vector<Point>> rings;
};

// Reads `(...)`, or else EMPTY, for which it returns false, as `read` reads each item that commas
// separate.
template <typename Read>
absl::StatusOr<bool> ReadItems(WktReader& reader, Read read) {
  if (reader.Word("EMPTY")) {
    return false;
  }
  GOOGLESQL_RETURN_IF_ERROR(reader.Expect("(", "'(' or EMPTY"));
  while (true) {
    GOOGLESQL_RETURN_IF_ERROR(read());
    if (!reader.Word(",")) {
      break;
    }
  }
  GOOGLESQL_RETURN_IF_ERROR(reader.Expect(")", "',' or ')'"));
  return true;
}

// Reads a polygon's rings, or EMPTY, adding the polygon to `parts`.
absl::Status ReadPolygon(WktReader& reader, std::vector<Part>& parts) {
  Part polygon{.type = Part::Type::kPolygon, .rings = {}};
  GOOGLESQL_ASSIGN_OR_RETURN(const bool nonempty, ReadItems(reader, [&]() -> absl::Status {
                               GOOGLESQL_ASSIGN_OR_RETURN(
                                   std::vector<Point> ring,
                                   ReadVertices(reader, "'(' or EMPTY", true));
                               polygon.rings.push_back(std::move(ring));
                               return absl::OkStatus();
                             }));
  if (nonempty) {
    parts.push_back(std::move(polygon));
  }
  return absl::OkStatus();
}

// Reads a linestring's vertices, or EMPTY, adding the linestring to `parts`.
absl::Status ReadLine(WktReader& reader, std::vector<Part>& parts) {
  if (reader.Word("EMPTY")) {
    return absl::OkStatus();
  }
  GOOGLESQL_ASSIGN_OR_RETURN(std::vector<Point> vertices,
                             ReadVertices(reader, "'(' or EMPTY", true));
  parts.push_back({.type = Part::Type::kLineString, .rings = {std::move(vertices)}});
  return absl::OkStatus();
}

// Reads a geometry of any type, adding its points, linestrings and polygons to `parts`. A
// MULTIPOINT's points may be in parentheses or not.
absl::Status ReadGeometry(WktReader& reader, std::vector<Part>& parts) {
  if (reader.Word("FULLGLOBE")) {
    parts.push_back({.type = Part::Type::kFull, .rings = {}});
    return absl::OkStatus();
  }
  if (reader.Word("POINT")) {
    if (reader.Word("EMPTY")) {
      return absl::OkStatus();
    }
    GOOGLESQL_ASSIGN_OR_RETURN(std::vector<Point> point,
                               ReadVertices(reader, "'(' or EMPTY", false));
    parts.push_back({.type = Part::Type::kPoint, .rings = {std::move(point)}});
    return absl::OkStatus();
  }
  if (reader.Word("LINESTRING")) {
    return ReadLine(reader, parts);
  }
  if (reader.Word("POLYGON")) {
    return ReadPolygon(reader, parts);
  }
  if (reader.Word("MULTIPOINT")) {
    return ReadItems(
               reader,
               [&]() -> absl::Status {
                 if (reader.Word("EMPTY")) {
                   return absl::OkStatus();
                 }
                 std::vector<Point> point;
                 if (reader.Word("(")) {
                   GOOGLESQL_ASSIGN_OR_RETURN(const double longitude, reader.Number("longitude"));
                   GOOGLESQL_ASSIGN_OR_RETURN(const double latitude, reader.Number("latitude"));
                   point.push_back({.longitude = longitude, .latitude = latitude});
                   GOOGLESQL_RETURN_IF_ERROR(reader.Expect(")"));
                 } else {
                   GOOGLESQL_ASSIGN_OR_RETURN(const double longitude, reader.Number("longitude"));
                   GOOGLESQL_ASSIGN_OR_RETURN(const double latitude, reader.Number("latitude"));
                   point.push_back({.longitude = longitude, .latitude = latitude});
                 }
                 parts.push_back({.type = Part::Type::kPoint, .rings = {std::move(point)}});
                 return absl::OkStatus();
               })
        .status();
  }
  if (reader.Word("MULTILINESTRING")) {
    return ReadItems(reader, [&] { return ReadLine(reader, parts); }).status();
  }
  if (reader.Word("MULTIPOLYGON")) {
    return ReadItems(reader, [&] { return ReadPolygon(reader, parts); }).status();
  }
  if (reader.Word("GEOMETRYCOLLECTION")) {
    return ReadItems(reader, [&] { return ReadGeometry(reader, parts); }).status();
  }
  if (!reader.Letter()) {
    return reader.Unexpected();
  }
  return absl::UnimplementedError("The emulator does not support ST_GEOGFROMTEXT of " +
                                  std::string(reader.Text()));
}

// The geography of a part whose coordinates are in range, built as BigQuery builds it.
absl::StatusOr<Geography> Build(const Part& part, bool oriented) {
  std::vector<std::vector<S2Point>> loops;
  for (const std::vector<Point>& ring : part.rings) {
    std::vector<S2Point>& vertices = loops.emplace_back(ring.size());
    std::ranges::transform(ring, vertices.begin(), ToS2Point);
  }
  switch (part.type) {
    case Part::Type::kFull:
      return FullGeography();
    case Part::Type::kPoint:
      return PointGeography(loops.front().front());
    case Part::Type::kLineString: {
      const std::vector<S2Point>& vertices = loops.front();
      if (vertices.size() < 2) {
        return absl::OutOfRangeError(
            "ST_GeogFromText failed: Linestring should have at least two points, but had " +
            Points(vertices.size()));
      }
      GOOGLESQL_RETURN_IF_ERROR(CheckAntipodal(part.rings.front(), "ST_GeogFromText failed: "));
      return BuildLine(vertices);
    }
    case Part::Type::kPolygon:
      break;
  }
  for (std::vector<S2Point>& loop : loops) {
    if (loop.size() < 4) {
      return absl::OutOfRangeError(
          "ST_GeogFromText failed: Polygon loop should have at least four vertices, but only "
          "had " +
          std::to_string(loop.size()));
    }
    if (loop.front() != loop.back()) {
      return absl::OutOfRangeError(
          "ST_GeogFromText failed: Polygon loop should be a closed ring (last point the same as "
          "first)");
    }
    loop.pop_back();
  }
  if (oriented && std::ranges::any_of(loops, [&](const std::vector<S2Point>& loop) {
        return S2::IsNormalized(loop) != (&loop == &loops.front());
      })) {
    return absl::UnimplementedError(
        "The emulator does not support GEOGRAPHY values of polygons whose shell runs clockwise "
        "or whose holes run counterclockwise");
  }
  return BuildPolygon(loops, "ST_GeogFromText failed: ");
}

}  // namespace

absl::StatusOr<Geography> ReadWkt(std::string_view text, bool oriented) {
  WktReader reader(text);
  std::vector<Part> parts;
  GOOGLESQL_RETURN_IF_ERROR(ReadGeometry(reader, parts));
  GOOGLESQL_RETURN_IF_ERROR(reader.End());
  for (Part& part : parts) {
    for (std::vector<Point>& ring : part.rings) {
      for (Point& vertex : ring) {
        if (std::abs(vertex.latitude) > 90) {
          return absl::OutOfRangeError(
              "ST_GeogFromText failed: Latitude must be between -90 and 90 degrees. Actual value "
              "was " +
              Coordinate(vertex.latitude));
        }
        vertex.longitude = std::remainder(vertex.longitude, 360.0);
      }
    }
  }
  std::vector<Geography> built;
  for (const Part& part : parts) {
    GOOGLESQL_ASSIGN_OR_RETURN(Geography geography, Build(part, oriented));
    built.push_back(std::move(geography));
  }
  if (built.empty()) {
    return Geography{};
  }
  return Combine(built);
}

}  // namespace bigquery_emulator_duckdb::backend_functions
