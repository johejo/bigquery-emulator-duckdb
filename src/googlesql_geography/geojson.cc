#include "src/googlesql_geography/geojson.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "googlesql/base/status_macros.h"
#include "nlohmann/json.hpp"
#include "s2/r2.h"
#include "src/googlesql_geography/planar.h"
#include "src/googlesql_geography/value.h"

namespace googlesql_geography {
namespace {

using nlohmann::json;

absl::Status Failed(std::string_view message) {
  return absl::OutOfRangeError(std::string(kGeoJsonFailed) + std::string(message));
}

// Longitudes and latitudes in degrees, but without wrapping: BigQuery takes an edge from longitude
// 170 to -170 the long way, through 0.
class PlanarProjection final : public DegreesProjection {
 public:
  [[nodiscard]] R2Point wrap_distance() const override { return {0, 0}; }
};

const PlanarProjection& Projection() {
  static const auto* const kProjection = new PlanarProjection();
  return *kProjection;
}

absl::StatusOr<R2Point> ReadPosition(const json& position) {
  if (!position.is_array()) {
    return Failed("Point coordinates should be an array");
  }
  if (position.size() < 2) {
    return Failed("Point coordinates should be an array with at least two elements");
  }
  if (position.size() > 2) {
    return Failed("Point coordinates should be an array with no more than two elements");
  }
  for (const auto& [index, name] : {std::pair{0, "Longitude"}, std::pair{1, "Latitude"}}) {
    if (!position.at(index).is_number() || !std::isfinite(position.at(index).get<double>())) {
      return Failed(std::string(name) + " should be a number");
    }
  }
  const double latitude = position.at(1).get<double>();
  if (std::abs(latitude) > 90) {
    return Failed("Latitude must be between -90 and 90 degrees. Actual value was " +
                  Coordinate(latitude));
  }
  return R2Point(position.at(0).get<double>(), latitude);
}

absl::StatusOr<std::vector<R2Point>> ReadPositions(const json& positions) {
  std::vector<R2Point> result;
  for (const json& position : positions) {
    GOOGLESQL_ASSIGN_OR_RETURN(const R2Point point, ReadPosition(position));
    result.push_back(point);
  }
  return result;
}

absl::StatusOr<Geography> ReadPoint(const json& coordinates) {
  GOOGLESQL_ASSIGN_OR_RETURN(const R2Point position, ReadPosition(coordinates));
  return PointGeography(Projection().Unproject(position));
}

absl::StatusOr<Geography> ReadLine(const json& coordinates) {
  GOOGLESQL_ASSIGN_OR_RETURN(const std::vector<R2Point> positions, ReadPositions(coordinates));
  return PlanarLine(positions, kGeoJsonFailed);
}

// A polygon is the region its rings enclose on the plane. Each loop runs as the ring does, and
// where the ring runs clockwise, in reverse as S2Loop::Invert reverses it, so that the region it
// adds or removes lies on its left.
absl::StatusOr<Geography> ReadPolygon(const json& coordinates) {
  std::vector<std::vector<R2Point>> rings;
  for (const json& ring : coordinates) {
    GOOGLESQL_ASSIGN_OR_RETURN(auto positions, ReadPositions(ring));
    rings.push_back(std::move(positions));
  }
  return PlanarPolygon(rings, kGeoJsonFailed);
}

// A geometry type other than GeometryCollection, whose coordinates are one part as `read` reads
// it, or an array of them where it is `multipart`.
struct GeometryType {
  std::string_view name;
  absl::StatusOr<Geography> (*read)(const json&);
  bool multipart;
};

// The names in lower case, since BigQuery takes them in any case.
constexpr std::array kGeometryTypes = {
    GeometryType{.name = "point", .read = ReadPoint, .multipart = false},
    GeometryType{.name = "multipoint", .read = ReadPoint, .multipart = true},
    GeometryType{.name = "linestring", .read = ReadLine, .multipart = false},
    GeometryType{.name = "multilinestring", .read = ReadLine, .multipart = true},
    GeometryType{.name = "polygon", .read = ReadPolygon, .multipart = false},
    GeometryType{.name = "multipolygon", .read = ReadPolygon, .multipart = true},
};

// Adds the parts of `geometry` to `parts`, each built on its own.
absl::Status ReadGeometry(const json& geometry, std::vector<Geography>& parts) {
  if (!geometry.is_object()) {
    return Failed("Expected JSON object");
  }
  const auto type = geometry.find("type");
  if (type == geometry.end() || !type->is_string()) {
    return Failed("GeoJson object should have 'type' member");
  }
  std::string name = type->get<std::string>();
  std::ranges::transform(name, name.begin(),
                         [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (name == "feature" || name == "featurecollection") {
    return Failed(
        "GeoJSON reader accepts Geometry objects, not Feature or FeatureCollection objects");
  }
  const auto* const known = std::ranges::find(kGeometryTypes, name, &GeometryType::name);
  if (name != "geometrycollection" && known == kGeometryTypes.end()) {
    return Failed("Unknown GeoJSON element '" + type->get<std::string>() + "'");
  }
  const std::string member = known == kGeometryTypes.end() ? "geometries" : "coordinates";
  const auto items = geometry.find(member);
  if (items == geometry.end()) {
    return Failed("Missing member '" + member + "'");
  }
  for (const auto& [key, value] : geometry.items()) {
    if (key != "type" && key != member) {
      return Failed("Unknown member '" + key + "'");
    }
  }
  if (!items->is_array()) {
    return Failed("Member '" + member + "' should be an array");
  }
  if (known == kGeometryTypes.end()) {
    for (const json& item : *items) {
      GOOGLESQL_RETURN_IF_ERROR(ReadGeometry(item, parts));
    }
    return absl::OkStatus();
  }
  if (!known->multipart) {
    GOOGLESQL_ASSIGN_OR_RETURN(Geography part, known->read(*items));
    parts.push_back(std::move(part));
    return absl::OkStatus();
  }
  for (const json& item : *items) {
    GOOGLESQL_ASSIGN_OR_RETURN(Geography part, known->read(item));
    parts.push_back(std::move(part));
  }
  return absl::OkStatus();
}

// `text` with each comma that follows a value and comes before a closing bracket or brace replaced
// with a space, since BigQuery's parser takes them and nlohmann's does not; positions stay where
// they were.
std::string WithoutTrailingCommas(std::string_view text) {
  std::string result(text);
  bool quoted = false;
  // The last character outside strings and spaces, and the comma it is, if it follows a value.
  char previous = 0;
  size_t comma = std::string::npos;
  for (size_t i = 0; i < result.size(); ++i) {
    const char c = result.at(i);
    if (quoted) {
      if (c == '\\') {
        ++i;
      } else if (c == '"') {
        quoted = false;
      }
      continue;
    }
    if (std::isspace(static_cast<unsigned char>(c)) != 0) {
      continue;
    }
    if ((c == '}' || c == ']') && comma != std::string::npos) {
      result.at(comma) = ' ';
    }
    comma =
        c == ',' && previous != ',' && previous != '[' && previous != '{' ? i : std::string::npos;
    quoted = c == '"';
    previous = c;
  }
  return result;
}

// The JSON of `text`, failing as BigQuery does with what went wrong and up to 20 characters of
// the text from ten before the token where it did.
absl::StatusOr<json> Parse(std::string_view text) {
  const auto failed = [&](std::string_view what, size_t position) {
    constexpr size_t kBefore = 10;
    constexpr size_t kLength = 20;
    return Failed(std::string(what) + ": " +
                  std::string(text.substr(position - std::min(position, kBefore), kLength)));
  };
  try {
    return json::parse(WithoutTrailingCommas(text));
  } catch (const json::parse_error& error) {
    // nlohmann_json counts the characters it read, the last of them where it failed, which is in
    // or after a literal or a number that does not parse; BigQuery points to its start.
    const std::string_view message = error.what();
    const auto says = [&](std::string_view what) {
      return message.find(what) != std::string_view::npos;
    };
    size_t position = std::min(error.byte == 0 ? 0 : error.byte - 1, text.size());
    if (says("invalid literal") || says("invalid number")) {
      while (position > 0 &&
             (std::isalnum(static_cast<unsigned char>(text.at(position - 1))) != 0 ||
              std::string_view(".+-").find(text.at(position - 1)) != std::string_view::npos)) {
        --position;
      }
    }
    if (says("unexpected end of input") || position == text.size()) {
      return failed("Unexpected end of string", text.size());
    }
    if (says("expected end of input")) {
      return failed("Parser terminated before end of string", position);
    }
    if (says("invalid number")) {
      return failed("Could not parse number", position);
    }
    if (says("object key")) {
      return failed("Expected key", position);
    }
    return failed("Unexpected token", position);
  } catch (const json::out_of_range& error) {
    // nlohmann_json names the number that overflows a double, but not where it is.
    const std::string_view message = error.what();
    const size_t open = message.find('\'');
    const size_t close = message.rfind('\'');
    const size_t position =
        open < close ? text.find(message.substr(open + 1, close - open - 1)) : std::string::npos;
    return failed("Could not parse number", std::min(position, text.size()));
  }
}

}  // namespace

absl::StatusOr<Geography> ReadGeoJson(std::string_view text) {
  GOOGLESQL_ASSIGN_OR_RETURN(const json geometry, Parse(text));
  std::vector<Geography> parts;
  GOOGLESQL_RETURN_IF_ERROR(ReadGeometry(geometry, parts));
  if (parts.empty()) {
    return Geography{};
  }
  return Combine(parts);
}

}  // namespace googlesql_geography
