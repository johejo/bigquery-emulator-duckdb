#include "src/googlesql_geography/wkb.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "googlesql/base/status_macros.h"
#include "s2/s2point.h"
#include "src/googlesql_geography/parts.h"
#include "src/googlesql_geography/value.h"

namespace googlesql_geography {
namespace {

// The WKB geometry types BigQuery reads, and the names of all the types its errors name.
constexpr uint32_t kWkbPoint = 1;
constexpr uint32_t kWkbLineString = 2;
constexpr uint32_t kWkbPolygon = 3;
constexpr uint32_t kWkbMultiPoint = 4;
constexpr uint32_t kWkbMultiLineString = 5;
constexpr uint32_t kWkbMultiPolygon = 6;
constexpr uint32_t kWkbGeometryCollection = 7;
constexpr std::array<std::string_view, 19> kWkbTypeNames = {
    "Geometry",          "Point",        "LineString",         "Polygon",        "MultiPoint",
    "MultiLineString",   "MultiPolygon", "GeometryCollection", "CircularString", "CompoundCurve",
    "CurvePolygon",      "MultiCurve",   "MultiSurface",       "Curve",          "Surface",
    "PolyhedralSurface", "TIN",          "Triangle",           "Circle",
};

// EWKB's flags in a geometry type.
constexpr uint32_t kEwkbZ = 0x80000000;
constexpr uint32_t kEwkbM = 0x40000000;
constexpr uint32_t kEwkbSrid = 0x20000000;
// The SRID of WGS84, the only one BigQuery's coordinates are in.
constexpr uint32_t kWgs84 = 4326;

std::string Hex(uint32_t value) { return std::format("{:#x}", value); }

// Reads WKB geometries, each in the byte order its first byte gives, failing as BigQuery does
// while it parses them.
class WkbReader {
 public:
  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the bytes and their errors' prefix.
  WkbReader(std::string_view bytes, std::string_view failed) : bytes_(bytes), failed_(failed) {}

  // Reads a geometry of any type or, where `expected` is given, only of that type, adding its
  // points, linestrings and polygons to `parts`.
  absl::Status Geometry(std::vector<Part>& parts, std::optional<uint32_t> expected = {}) {
    GOOGLESQL_ASSIGN_OR_RETURN(const uint8_t order, Byte("byte order"));
    if (order > 1) {
      return Failed("Invalid WKB byte order '" + std::to_string(order) + "'");
    }
    little_ = order == 1;
    GOOGLESQL_ASSIGN_OR_RETURN(uint32_t type, Uint32("object type"));
    if ((type & (kEwkbZ | kEwkbM)) != 0) {
      return Failed("Unknown object type " + Hex(type) + ", eWKB extensions M and Z not supported");
    }
    if ((type & kEwkbSrid) != 0) {
      type &= ~kEwkbSrid;
      GOOGLESQL_ASSIGN_OR_RETURN(const uint32_t srid, Uint32("SRID"));
      if (srid != kWgs84) {
        return Failed("Unsupported SRID " + std::to_string(srid) + ", only SRID " +
                      std::to_string(kWgs84) + " is supported");
      }
    }
    if (type >= kWkbTypeNames.size()) {
      return Failed("Unknown object type " + Hex(type));
    }
    if (expected && type != *expected) {
      return Failed("Expected object of type '" + std::string(kWkbTypeNames.at(*expected)) +
                    "', but actual was '" + std::string(kWkbTypeNames.at(type)) + "'");
    }
    if (type < kWkbPoint || type > kWkbGeometryCollection) {
      return Failed("Unsupported object type " + std::string(kWkbTypeNames.at(type)));
    }
    switch (type) {
      case kWkbPoint: {
        GOOGLESQL_ASSIGN_OR_RETURN(const Point point, Vertex());
        if (!std::isnan(point.longitude) || !std::isnan(point.latitude)) {
          parts.push_back({.type = Part::Type::kPoint, .rings = {{point}}});
        }
        return absl::OkStatus();
      }
      case kWkbLineString: {
        GOOGLESQL_ASSIGN_OR_RETURN(std::vector<Point> vertices, Vertices("number of line points"));
        if (!vertices.empty()) {
          parts.push_back({.type = Part::Type::kLineString, .rings = {std::move(vertices)}});
        }
        return absl::OkStatus();
      }
      case kWkbPolygon: {
        Part polygon{.type = Part::Type::kPolygon, .rings = {}};
        GOOGLESQL_ASSIGN_OR_RETURN(const uint32_t rings, Uint32("number of polygon rings"));
        // Each ring takes at least its count's four bytes.
        if (rings > Remaining() / 4) {
          return Failed("Inconsistent WKB polygon rings");
        }
        for (uint32_t i = 0; i < rings; ++i) {
          GOOGLESQL_ASSIGN_OR_RETURN(std::vector<Point> ring, Vertices("number of ring points"));
          polygon.rings.push_back(std::move(ring));
        }
        if (!polygon.rings.empty()) {
          parts.push_back(std::move(polygon));
        }
        return absl::OkStatus();
      }
      default:
        break;
    }
    // A multipart type's parts are of the type it is offset from; a collection's of any type.
    std::optional<uint32_t> part_type;
    if (type != kWkbGeometryCollection) {
      part_type = type - (kWkbMultiPoint - kWkbPoint);
    }
    GOOGLESQL_ASSIGN_OR_RETURN(const uint32_t count, Uint32("number of objects in collection"));
    for (uint32_t i = 0; i < count; ++i) {
      GOOGLESQL_RETURN_IF_ERROR(Geometry(parts, part_type));
    }
    return absl::OkStatus();
  }

  absl::Status End() const {
    if (position_ != bytes_.size()) {
      return absl::OutOfRangeError(std::string(failed_) +
                                   "Extra bytes left in input after WKB parsing");
    }
    return absl::OkStatus();
  }

 private:
  [[nodiscard]] absl::Status Failed(const std::string& message) const {
    return absl::OutOfRangeError(std::string(failed_) + message + "; during WKB parsing");
  }

  absl::StatusOr<uint8_t> Byte(std::string_view what) {
    if (position_ >= bytes_.size()) {
      return Truncated(what);
    }
    return static_cast<uint8_t>(bytes_.at(position_++));
  }

  template <typename T>
  absl::StatusOr<T> Number(std::string_view what) {
    if (bytes_.size() - position_ < sizeof(T)) {
      return Truncated(what);
    }
    T value = 0;
    for (size_t i = 0; i < sizeof(T); ++i) {
      const auto byte =
          static_cast<uint8_t>(bytes_.at(position_ + (little_ ? i : sizeof(T) - 1 - i)));
      value |= static_cast<T>(byte) << (8 * i);
    }
    position_ += sizeof(T);
    return value;
  }

  absl::StatusOr<uint32_t> Uint32(std::string_view what) { return Number<uint32_t>(what); }

  absl::StatusOr<double> Double(std::string_view what) {
    GOOGLESQL_ASSIGN_OR_RETURN(const uint64_t bits, Number<uint64_t>(what));
    return std::bit_cast<double>(bits);
  }

  absl::StatusOr<Point> Vertex() {
    GOOGLESQL_ASSIGN_OR_RETURN(const double longitude, Double("longitude"));
    GOOGLESQL_ASSIGN_OR_RETURN(const double latitude, Double("latitude"));
    return Point{.longitude = longitude, .latitude = latitude};
  }

  [[nodiscard]] size_t Remaining() const { return bytes_.size() - position_; }

  // A point list, whose count, which `what` names, must fit in what is left.
  absl::StatusOr<std::vector<Point>> Vertices(std::string_view what) {
    GOOGLESQL_ASSIGN_OR_RETURN(const uint32_t count, Uint32(what));
    if (count > Remaining() / 16) {
      return Failed("Inconsistent WKB point list.");
    }
    std::vector<Point> vertices;
    vertices.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
      GOOGLESQL_ASSIGN_OR_RETURN(const Point vertex, Vertex());
      vertices.push_back(vertex);
    }
    return vertices;
  }

  [[nodiscard]] absl::Status Truncated(std::string_view what) const {
    return Failed("End of input while reading " + std::string(what));
  }

  std::string_view bytes_;
  std::string_view failed_;
  size_t position_ = 0;
  bool little_ = true;
};

// Fails where a coordinate is not finite, as BigQuery does once it has parsed the WKB, checking
// each vertex's latitude before its longitude.
absl::Status CheckFinite(const std::vector<Part>& parts, std::string_view failed) {
  for (const Part& part : parts) {
    for (const std::vector<Point>& ring : part.rings) {
      for (const Point& vertex : ring) {
        for (const auto& [value, name] :
             {std::pair{vertex.latitude, "latitude"}, std::pair{vertex.longitude, "longitude"}}) {
          if (!std::isfinite(value)) {
            return absl::OutOfRangeError(std::string(failed) + "Value of " + name + " cannot be " +
                                         (std::isnan(value) ? "NaN" : "+-inf"));
          }
        }
      }
    }
  }
  return absl::OkStatus();
}

// Writes little-endian WKB.
class WkbWriter {
 public:
  void Header(uint32_t type) {
    out_.push_back('\x01');
    Uint32(type);
  }

  void Uint32(uint32_t value) {
    for (size_t i = 0; i < sizeof(value); ++i) {
      out_.push_back(static_cast<char>((value >> (8 * i)) & 0xffU));
    }
  }

  void Vertex(const S2Point& vertex) {
    const Point point = ToPoint(vertex);
    for (const double value : {point.longitude, point.latitude}) {
      const auto bits = std::bit_cast<uint64_t>(value);
      for (size_t i = 0; i < sizeof(bits); ++i) {
        out_.push_back(static_cast<char>((bits >> (8 * i)) & 0xffU));
      }
    }
  }

  void Vertices(const std::vector<S2Point>& vertices) {
    Uint32(static_cast<uint32_t>(vertices.size()));
    for (const S2Point& vertex : vertices) {
      Vertex(vertex);
    }
  }

  void WritePoint(const S2Point& point) {
    Header(kWkbPoint);
    Vertex(point);
  }

  void WriteLine(const std::vector<S2Point>& line) {
    Header(kWkbLineString);
    Vertices(line);
  }

  // Closed rings, with holes running clockwise, as Wkt writes them.
  void WritePolygon(const Polygon& polygon) {
    Header(kWkbPolygon);
    Uint32(static_cast<uint32_t>(polygon.size()));
    for (const std::vector<S2Point>& loop : polygon) {
      std::vector<S2Point> ring = loop;
      if (&loop != polygon.data()) {
        std::ranges::reverse(ring);
      }
      ring.push_back(ring.front());
      Vertices(ring);
    }
  }

  // `parts` as the one part or a `many` of more than one, each as `write` writes it.
  template <typename Item, typename Write>
  void Parts(const std::vector<Item>& parts, uint32_t many, Write write) {
    if (parts.size() == 1) {
      write(parts.front());
      return;
    }
    Header(many);
    Uint32(static_cast<uint32_t>(parts.size()));
    for (const Item& part : parts) {
      write(part);
    }
  }

  std::string Take() { return std::move(out_); }

 private:
  std::string out_;
};

}  // namespace

absl::StatusOr<Geography> ReadWkb(std::string_view bytes, std::string_view failed, bool oriented) {
  WkbReader reader(bytes, failed);
  std::vector<Part> parts;
  GOOGLESQL_RETURN_IF_ERROR(reader.Geometry(parts));
  GOOGLESQL_RETURN_IF_ERROR(reader.End());
  // Errors in the geography the WKB describes name it.
  const absl::Status status = CheckFinite(parts, failed);
  absl::StatusOr<Geography> geography =
      status.ok() ? BuildParts(std::move(parts), oriented, failed) : status;
  if (!geography.ok() && geography.status().code() == absl::StatusCode::kOutOfRange) {
    return absl::OutOfRangeError(std::string(geography.status().message()) + "; in WKB geography");
  }
  return geography;
}

std::string Wkb(const Geography& geography) {
  WkbWriter writer;
  if (geography.Empty()) {
    writer.Header(kWkbGeometryCollection);
    writer.Uint32(0);
    return writer.Take();
  }
  if (geography.Full()) {
    // The polygons Wkt writes for the full geography.
    const auto vertex = [](double longitude) {
      return ToS2Point({.longitude = longitude, .latitude = 0});
    };
    writer.Header(kWkbGeometryCollection);
    writer.Uint32(2);
    writer.WritePolygon({{vertex(0), vertex(120), vertex(-120)}});
    writer.WritePolygon({{vertex(0), vertex(-120), vertex(120)}});
    return writer.Take();
  }
  // A collection holds each point, linestring and polygon on its own.
  const int kinds = static_cast<int>(!geography.points.empty()) +
                    static_cast<int>(!geography.lines.empty()) +
                    static_cast<int>(!geography.polygons.empty());
  if (kinds > 1) {
    writer.Header(kWkbGeometryCollection);
    writer.Uint32(static_cast<uint32_t>(geography.Count()));
    std::ranges::for_each(geography.points,
                          [&](const S2Point& point) { writer.WritePoint(point); });
    std::ranges::for_each(geography.lines,
                          [&](const std::vector<S2Point>& line) { writer.WriteLine(line); });
    std::ranges::for_each(geography.polygons,
                          [&](const Polygon& polygon) { writer.WritePolygon(polygon); });
    return writer.Take();
  }
  if (!geography.points.empty()) {
    writer.Parts(geography.points, kWkbMultiPoint,
                 [&](const S2Point& point) { writer.WritePoint(point); });
  }
  if (!geography.lines.empty()) {
    writer.Parts(geography.lines, kWkbMultiLineString,
                 [&](const std::vector<S2Point>& line) { writer.WriteLine(line); });
  }
  if (!geography.polygons.empty()) {
    writer.Parts(geography.polygons, kWkbMultiPolygon,
                 [&](const Polygon& polygon) { writer.WritePolygon(polygon); });
  }
  return writer.Take();
}

}  // namespace googlesql_geography
