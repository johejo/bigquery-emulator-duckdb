#include "src/googlesql_geography_duckdb/codec.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
// The index headers declare Encoder, Decoder and Varint in their API; @s2geometry keeps
// s2/util/coding, which defines them, private.
#include "s2/encoded_s2shape_index.h"
#include "s2/mutable_s2shape_index.h"
#include "s2/s2point.h"
#include "s2/s2shape.h"
#include "s2/s2shape_index.h"
#include "src/googlesql_geography/value.h"

namespace bigquery_emulator_duckdb::geography_duckdb {
using googlesql_geography::Coordinate;
using googlesql_geography::Geography;
using googlesql_geography::IndexedGeography;
using googlesql_geography::Polygon;
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

// The first byte of every encoding, which a change of the format changes.
constexpr unsigned char kVersion = 1;

void PutVertices(const std::vector<S2Point>& vertices, Encoder& encoder) {
  encoder.Ensure(Varint::kMax64 + (vertices.size() * 3 * sizeof(double)));
  encoder.put_varint64(vertices.size());
  for (const S2Point& vertex : vertices) {
    encoder.putdouble(vertex.x());
    encoder.putdouble(vertex.y());
    encoder.putdouble(vertex.z());
  }
}

void PutCount(size_t count, Encoder& encoder) {
  encoder.Ensure(Varint::kMax64);
  encoder.put_varint64(count);
}

std::optional<size_t> GetCount(Decoder& decoder) {
  uint64_t count = 0;
  if (!decoder.get_varint64(&count)) {
    return std::nullopt;
  }
  return count;
}

std::optional<std::vector<S2Point>> GetVertices(Decoder& decoder) {
  const std::optional<size_t> count = GetCount(decoder);
  if (!count || *count > decoder.avail() / (3 * sizeof(double))) {
    return std::nullopt;
  }
  std::vector<S2Point> vertices(*count);
  for (S2Point& vertex : vertices) {
    const double x = decoder.getdouble();
    const double y = decoder.getdouble();
    vertex = S2Point(x, y, decoder.getdouble());
  }
  return vertices;
}

absl::Status Invalid() { return absl::InvalidArgumentError("Invalid GEOGRAPHY value"); }

// Reads the geography at the start of `decoder`, leaving it at the index.
absl::StatusOr<Geography> GetGeography(Decoder& decoder) {
  if (decoder.avail() == 0 || decoder.get8() != kVersion) {
    return Invalid();
  }
  Geography geography;
  std::optional<std::vector<S2Point>> points = GetVertices(decoder);
  const std::optional<size_t> lines = points ? GetCount(decoder) : std::nullopt;
  if (!lines || *lines > decoder.avail()) {
    return Invalid();
  }
  geography.points = *std::move(points);
  for (size_t i = 0; i < *lines; ++i) {
    std::optional<std::vector<S2Point>> line = GetVertices(decoder);
    if (!line) {
      return Invalid();
    }
    geography.lines.push_back(*std::move(line));
  }
  const std::optional<size_t> polygons = GetCount(decoder);
  if (!polygons || *polygons > decoder.avail()) {
    return Invalid();
  }
  for (size_t i = 0; i < *polygons; ++i) {
    const std::optional<size_t> rings = GetCount(decoder);
    if (!rings || *rings > decoder.avail()) {
      return Invalid();
    }
    Polygon& polygon = geography.polygons.emplace_back();
    for (size_t j = 0; j < *rings; ++j) {
      std::optional<std::vector<S2Point>> ring = GetVertices(decoder);
      if (!ring) {
        return Invalid();
      }
      polygon.push_back(*std::move(ring));
    }
  }
  return geography;
}

// The shapes of a decoded geography, which its encoded index refers to by id.
class Shapes final : public S2ShapeIndex::ShapeFactory {
 public:
  explicit Shapes(std::shared_ptr<const Geography> geography) : geography_(std::move(geography)) {}

  [[nodiscard]] int size() const override { return ShapeCount(*geography_); }
  std::unique_ptr<S2Shape> operator[](int shape_id) const override {
    return Shape(*geography_, shape_id);
  }
  [[nodiscard]] std::unique_ptr<ShapeFactory> Clone() const override {
    return std::make_unique<Shapes>(geography_);
  }

 private:
  std::shared_ptr<const Geography> geography_;
};

// An encoded index and the encoding it reads.
struct EncodedIndex {
  std::string encoded;
  EncodedS2ShapeIndex index;
};

}  // namespace

std::string EncodeGeography(const Geography& geography) {
  Encoder encoder;
  encoder.Ensure(1);
  encoder.put8(kVersion);
  PutVertices(geography.points, encoder);
  PutCount(geography.lines.size(), encoder);
  for (const std::vector<S2Point>& line : geography.lines) {
    PutVertices(line, encoder);
  }
  PutCount(geography.polygons.size(), encoder);
  for (const Polygon& polygon : geography.polygons) {
    PutCount(polygon.size(), encoder);
    for (const std::vector<S2Point>& ring : polygon) {
      PutVertices(ring, encoder);
    }
  }
  if (!geography.Empty()) {
    MutableS2ShapeIndex index;
    AddShapes(geography, index);
    index.Encode(&encoder);
  }
  return {encoder.base(), encoder.length()};
}

absl::StatusOr<Geography> DecodeGeography(std::string_view encoded) {
  Decoder decoder(encoded.data(), encoded.size());
  return GetGeography(decoder);
}

absl::StatusOr<IndexedGeography> DecodeIndexedGeography(std::string encoded) {
  const auto owner = std::make_shared<EncodedIndex>();
  owner->encoded = std::move(encoded);
  Decoder decoder(owner->encoded.data(), owner->encoded.size());
  absl::StatusOr<Geography> geography = GetGeography(decoder);
  if (!geography.ok()) {
    return geography.status();
  }
  if (geography->Empty()) {
    return IndexedGeography(*std::move(geography));
  }
  auto shared = std::make_shared<const Geography>(*std::move(geography));
  if (!owner->index.Init(&decoder, Shapes(shared))) {
    return Invalid();
  }
  return IndexedGeography(std::move(shared),
                          std::shared_ptr<const S2ShapeIndex>(owner, &owner->index));
}

std::string GeographyText(const Geography& geography) {
  return Text(geography, [](const S2Point& vertex) {
    return Coordinate(vertex.x(), std::nullopt) + " " + Coordinate(vertex.y(), std::nullopt) + " " +
           Coordinate(vertex.z(), std::nullopt);
  });
}

}  // namespace bigquery_emulator_duckdb::geography_duckdb
