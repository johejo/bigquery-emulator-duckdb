#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "absl/status/statusor.h"
#include "s2/s2boolean_operation.h"
#include "src/googlesql_geography/options.h"
#include "src/googlesql_geography/value.h"

namespace googlesql_geography {

struct BoundingBox {
  double xmin;
  double ymin;
  double xmax;
  double ymax;
};

// SQL NULL arguments are handled by the caller. Optional results express NULL produced
// by the function itself, for example the distance to an empty geography. Functions that query
// their arguments' shapes as they are, such as the distances, take an IndexedGeography.
absl::StatusOr<Geography> GeogPoint(double longitude, double latitude);
absl::StatusOr<std::optional<double>> Azimuth(const Geography& a, const Geography& b);
absl::StatusOr<std::optional<double>> Angle(const Geography& a, const Geography& b,
                                            const Geography& c);
absl::StatusOr<std::optional<double>> Distance(const IndexedGeography& a, const IndexedGeography& b,
                                               bool use_spheroid, const std::string& function);
absl::StatusOr<std::optional<Geography>> ClosestPoint(const IndexedGeography& a,
                                                      const IndexedGeography& b, bool use_spheroid,
                                                      const std::string& function);
absl::StatusOr<bool> IntersectsBox(const Geography& geography, double west, double south,
                                   double east, double north);
absl::StatusOr<bool> DWithin(const IndexedGeography& a, const IndexedGeography& b, double limit,
                             bool use_spheroid, const std::string& function);
absl::StatusOr<std::optional<double>> HausdorffDistance(const IndexedGeography& a,
                                                        const IndexedGeography& b, bool directed);
absl::StatusOr<std::optional<bool>> HausdorffDWithin(const IndexedGeography& a,
                                                     const IndexedGeography& b, bool directed,
                                                     double limit);
absl::StatusOr<bool> Equals(const Geography& a, const Geography& b);
absl::StatusOr<std::optional<Geography>> ConvexHull(const Geography& geography);
absl::StatusOr<double> X(const Geography& geography);
absl::StatusOr<double> Y(const Geography& geography);
absl::StatusOr<Geography> SetOperation(const Geography& a, const Geography& b,
                                       S2BooleanOperation::OpType operation);
absl::StatusOr<Geography> Boundary(const Geography& geography);
absl::StatusOr<Geography> Buffer(const Geography& geography, double radius_meters,
                                 double num_seg_quarter_circle, bool use_spheroid,
                                 const std::string& endcap, const std::string& side);
absl::StatusOr<Geography> BufferWithTolerance(const Geography& geography, double radius_meters,
                                              double tolerance_meters, bool use_spheroid,
                                              const std::string& endcap, const std::string& side);
absl::StatusOr<Geography> Simplify(const Geography& geography, double tolerance_meters);
absl::StatusOr<Geography> SnapToGrid(const Geography& geography, double grid_size);
std::optional<BoundingBox> Bounds(const Geography& geography, const Options& options);
std::optional<BoundingBox> Extent(std::span<const Geography> geographies, const Options& options);
absl::StatusOr<Geography> Centroid(const Geography& geography);
std::optional<Geography> CentroidAggregate(const std::vector<Geography>& geographies);
absl::StatusOr<double> Length(const Geography& geography, bool use_spheroid,
                              const std::string& function);
absl::StatusOr<double> Measure(const Geography& geography, bool use_spheroid,
                               const std::string& function);
absl::StatusOr<std::string> AsGeoJson(const Geography& geography);
absl::StatusOr<Geography> PointN(const Geography& geography, int64_t index);
absl::StatusOr<Geography> LineInterpolatePoint(const Geography& geography, double fraction);
absl::StatusOr<double> LineLocatePoint(const Geography& geography,
                                       const Geography& point_geography);
absl::StatusOr<Geography> LineSubstring(const Geography& geography, double start, double end);
absl::StatusOr<std::optional<Geography>> UnionGeographies(
    const std::vector<Geography>& geographies);
absl::StatusOr<Geography> MakeLine(const std::vector<Geography>& geographies);
absl::StatusOr<Geography> MakePolygon(const std::vector<Geography>& geographies,
                                      bool oriented = false);
absl::StatusOr<std::vector<Geography>> Dump(const Geography& geography, int64_t dimension);
absl::StatusOr<std::optional<std::string>> Geohash(const Geography& geography, int64_t length);
absl::StatusOr<Geography> PointFromGeohash(const std::string& hash);
absl::StatusOr<int64_t> CellIdFromPoint(const Geography& geography, int64_t level);
absl::StatusOr<std::vector<int64_t>> CoveringCellIds(const IndexedGeography& geography,
                                                     int64_t min_level, int64_t max_level,
                                                     int64_t max_cells, double buffer);
absl::StatusOr<bool> Intersects(const Geography& a, const Geography& b);
absl::StatusOr<bool> Touches(const Geography& a, const Geography& b);
absl::StatusOr<bool> Covers(const Geography& a, const Geography& b);
absl::StatusOr<bool> Contains(const Geography& a, const Geography& b);
std::string GeometryType(const Geography& geography);
int64_t Dimension(const Geography& geography);
int64_t NumPoints(const Geography& geography);
bool IsClosed(const Geography& geography);
bool IsRing(const Geography& geography);
absl::StatusOr<Geography> ExteriorRing(const Geography& geography);
absl::StatusOr<std::vector<Geography>> InteriorRings(const Geography& geography);

}  // namespace googlesql_geography
