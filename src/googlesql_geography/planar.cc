#include "src/googlesql_geography/planar.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "s2/r2.h"
#include "s2/s2earth.h"
#include "s2/s2edge_tessellator.h"
#include "s2/s2point.h"
#include "src/googlesql_geography/value.h"

namespace googlesql_geography {
namespace {

// Do not wrap longitudes: a planar edge from 170 to -170 passes through zero.
class PlanarProjection final : public DegreesProjection {
 public:
  [[nodiscard]] R2Point wrap_distance() const override { return {0, 0}; }
};

std::vector<S2Point> Unproject(const std::vector<R2Point>& positions) {
  const PlanarProjection projection;
  const S2EdgeTessellator tessellator(&projection, S2Earth::MetersToAngle(10));
  std::vector<S2Point> vertices;
  for (size_t i = 0; i + 1 < positions.size(); ++i) {
    tessellator.AppendUnprojected(positions.at(i), positions.at(i + 1), &vertices);
  }
  return vertices;
}

}  // namespace

absl::StatusOr<Geography> PlanarLine(const std::vector<R2Point>& positions,
                                     std::string_view failed) {
  if (positions.size() < 2) {
    return absl::OutOfRangeError(std::string(failed) +
                                 "Linestring should have at least two points, but had " +
                                 Points(positions.size()));
  }
  return BuildLine(Unproject(positions));
}

absl::StatusOr<Geography> PlanarPolygon(const std::vector<std::vector<R2Point>>& rings,
                                        std::string_view failed, bool make_valid) {
  if (rings.empty()) {
    return absl::OutOfRangeError(std::string(failed) + "Polygon should have shell");
  }
  std::vector<std::vector<S2Point>> loops;
  for (auto positions : rings) {
    if (make_valid && positions.size() >= 3 && positions.front() != positions.back()) {
      positions.push_back(positions.front());
    }
    if (positions.size() < 4) {
      return absl::OutOfRangeError(
          std::string(failed) + "Polygon loop should have at least four vertices, but only had " +
          std::to_string(positions.size()));
    }
    if (positions.front() != positions.back()) {
      return absl::OutOfRangeError(
          std::string(failed) +
          "Polygon loop should be a closed ring (last point the same as first)");
    }
    double area = 0;
    for (size_t i = 0; i + 1 < positions.size(); ++i) {
      const R2Point& a = positions.at(i);
      const R2Point& b = positions.at(i + 1);
      if (std::abs(a.y()) == 90 && b.y() == a.y() && b.x() != a.x()) {
        return absl::UnimplementedError(
            "The emulator does not support planar polygons with an edge along a pole");
      }
      area += a.CrossProd(b);
    }
    auto& loop = loops.emplace_back(Unproject(positions));
    loop.pop_back();
    if (area < 0) {
      std::ranges::reverse(loop);
    }
  }
  const auto result = BuildPolygon(loops, failed, /*oriented=*/true);
  return make_valid && !result.ok() ? RepairPolygon(loops, failed) : result;
}

}  // namespace googlesql_geography
