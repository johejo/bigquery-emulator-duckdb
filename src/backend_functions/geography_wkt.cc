#include "src/backend_functions/geography_wkt.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "googlesql/base/status_macros.h"
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

}  // namespace

absl::StatusOr<Geography> ReadWkt(std::string_view text) {
  WktReader reader(text);
  const bool point = reader.Word("POINT");
  const bool line = !point && reader.Word("LINESTRING");
  const bool polygon = !point && !line && reader.Word("POLYGON");
  const bool geometry = point || line || polygon || reader.Word("MULTIPOINT") ||
                        reader.Word("MULTILINESTRING") || reader.Word("MULTIPOLYGON") ||
                        reader.Word("GEOMETRYCOLLECTION");
  if (geometry && reader.Word("EMPTY")) {
    GOOGLESQL_RETURN_IF_ERROR(reader.End());
    return Geography{};
  }
  if (!point && !line && !polygon) {
    return absl::UnimplementedError("The emulator does not support ST_GEOGFROMTEXT of " +
                                    std::string(text));
  }
  // A polygon's rings, or the vertices of a point or a linestring.
  std::vector<std::vector<Point>> rings;
  if (polygon) {
    GOOGLESQL_RETURN_IF_ERROR(reader.Expect("(", "'(' or EMPTY"));
    while (true) {
      GOOGLESQL_ASSIGN_OR_RETURN(std::vector<Point> ring,
                                 ReadVertices(reader, "'(' or EMPTY", true));
      rings.push_back(std::move(ring));
      if (!reader.Word(",")) {
        break;
      }
    }
    GOOGLESQL_RETURN_IF_ERROR(reader.Expect(")", "',' or ')'"));
  } else {
    GOOGLESQL_ASSIGN_OR_RETURN(std::vector<Point> vertices,
                               ReadVertices(reader, "'(' or EMPTY", line));
    rings.push_back(std::move(vertices));
  }
  GOOGLESQL_RETURN_IF_ERROR(reader.End());
  for (std::vector<Point>& ring : rings) {
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
  std::vector<std::vector<S2Point>> loops;
  for (const std::vector<Point>& ring : rings) {
    std::vector<S2Point>& vertices = loops.emplace_back(ring.size());
    std::ranges::transform(ring, vertices.begin(), ToS2Point);
  }
  if (polygon) {
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
    return BuildPolygon(loops, "ST_GeogFromText failed: ");
  }
  const std::vector<S2Point>& vertices = loops.front();
  if (!line) {
    return PointGeography(vertices.front());
  }
  if (vertices.size() < 2) {
    return absl::OutOfRangeError(
        "ST_GeogFromText failed: Linestring should have at least two points, but had " +
        Points(vertices.size()));
  }
  GOOGLESQL_RETURN_IF_ERROR(CheckAntipodal(rings.front(), "ST_GeogFromText failed: "));
  return BuildLine(vertices);
}

}  // namespace bigquery_emulator_duckdb::backend_functions
