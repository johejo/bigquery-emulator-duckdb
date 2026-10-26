#include "src/googlesql_geography/parts.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "googlesql/base/status_macros.h"
#include "s2/r2.h"
#include "s2/s2point.h"
#include "src/googlesql_geography/planar.h"
#include "src/googlesql_geography/value.h"

namespace googlesql_geography {
namespace {

// The geography of a part whose coordinates are in range, built as BigQuery builds it.
absl::StatusOr<Geography> Build(const Part& part, bool oriented, std::string_view failed) {
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
        return absl::OutOfRangeError(std::string(failed) +
                                     "Linestring should have at least two points, but had " +
                                     Points(vertices.size()));
      }
      GOOGLESQL_RETURN_IF_ERROR(CheckAntipodal(part.rings.front(), failed));
      return BuildLine(vertices);
    }
    case Part::Type::kPolygon:
      break;
  }
  for (std::vector<S2Point>& loop : loops) {
    if (loop.size() < 4) {
      return absl::OutOfRangeError(
          std::string(failed) + "Polygon loop should have at least four vertices, but only had " +
          std::to_string(loop.size()));
    }
    if (loop.front() != loop.back()) {
      return absl::OutOfRangeError(
          std::string(failed) +
          "Polygon loop should be a closed ring (last point the same as first)");
    }
    loop.pop_back();
  }
  // BuildPolygon stores holes as the regions they remove, whereas an oriented input ring
  // always has the polygon's interior on its left.
  if (oriented) {
    for (size_t i = 1; i < loops.size(); ++i) {
      std::ranges::reverse(loops.at(i));
    }
  }
  return BuildPolygon(loops, failed, oriented);
}

}  // namespace

absl::StatusOr<Geography> BuildParts(std::vector<Part> parts, bool oriented,
                                     std::string_view failed) {
  for (Part& part : parts) {
    for (std::vector<Point>& ring : part.rings) {
      for (Point& vertex : ring) {
        if (std::abs(vertex.latitude) > 90) {
          return absl::OutOfRangeError(
              std::string(failed) +
              "Latitude must be between -90 and 90 degrees. Actual value was " +
              Coordinate(vertex.latitude));
        }
        vertex.longitude = std::remainder(vertex.longitude, 360.0);
      }
    }
  }
  std::vector<Geography> built;
  for (const Part& part : parts) {
    GOOGLESQL_ASSIGN_OR_RETURN(Geography geography, Build(part, oriented, failed));
    built.push_back(std::move(geography));
  }
  if (built.empty()) {
    return Geography{};
  }
  return Combine(built);
}

absl::StatusOr<Geography> BuildPlanarParts(const std::vector<Part>& parts,
                                           std::string_view failed) {
  std::vector<Geography> built;
  for (const Part& part : parts) {
    std::vector<std::vector<R2Point>> rings;
    for (const auto& ring : part.rings) {
      auto& positions = rings.emplace_back();
      for (const Point& vertex : ring) {
        if (std::abs(vertex.latitude) > 90) {
          return absl::OutOfRangeError(
              std::string(failed) +
              "Latitude must be between -90 and 90 degrees. Actual value was " +
              Coordinate(vertex.latitude));
        }
        positions.emplace_back(vertex.longitude, vertex.latitude);
      }
    }
    absl::StatusOr<Geography> geography;
    switch (part.type) {
      case Part::Type::kPoint: {
        Point point = part.rings.front().front();
        point.longitude = std::remainder(point.longitude, 360.0);
        geography = PointGeography(ToS2Point(point));
        break;
      }
      case Part::Type::kLineString:
        geography = PlanarLine(rings.front(), failed);
        break;
      case Part::Type::kPolygon:
        geography = PlanarPolygon(rings, failed);
        break;
      case Part::Type::kFull:
        geography = FullGeography();
        break;
    }
    GOOGLESQL_RETURN_IF_ERROR(geography.status());
    built.push_back(std::move(*geography));
  }
  return built.empty() ? absl::StatusOr<Geography>(Geography{}) : Combine(built);
}

}  // namespace googlesql_geography
