#include "src/googlesql_s2geometry_duckdb/codec.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "s2/s2point.h"
#include "src/googlesql_s2geometry/value.h"

namespace bigquery_emulator_duckdb::geography_duckdb {
using googlesql_s2geometry::Coordinate;
using googlesql_s2geometry::FullGeography;
using googlesql_s2geometry::Geography;
using googlesql_s2geometry::Polygon;
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

// Internal XYZ text, preserving S2 vertices and polygon orientation exactly.
template <typename Vertex>
std::string Text(const Geography& geography, Vertex vertex) {
  if (geography.Empty()) {
    return std::string(kEmpty);
  }
  if (geography.Full()) {
    return "fullglobe";
  }
  const auto vertices = [&](const std::vector<S2Point>& line) {
    return "(" + List(line, vertex) + ")";
  };
  const auto rings = [&](const Polygon& polygon) {
    std::vector<std::string> items(polygon.size());
    std::ranges::transform(polygon, items.begin(), vertices);
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

std::string GeographyText(const Geography& geography) {
  return Text(geography, [](const S2Point& vertex) {
    return Coordinate(vertex.x(), std::nullopt) + " " + Coordinate(vertex.y(), std::nullopt) + " " +
           Coordinate(vertex.z(), std::nullopt);
  });
}

absl::StatusOr<Geography> ParseGeography(std::string_view text) {
  Geography geography;
  if (text == "fullglobe") {
    return FullGeography();
  }
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

}  // namespace bigquery_emulator_duckdb::geography_duckdb
