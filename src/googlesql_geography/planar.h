#pragma once

#include <string_view>
#include <vector>

#include "absl/status/statusor.h"
#include "s2/r2.h"
#include "src/googlesql_geography/value.h"

namespace googlesql_geography {

// Build spherical geographies from unwrapped longitude/latitude planar edges, with
// at most ten metres of tessellation error. Polygon interiors are defined on the plane.
absl::StatusOr<Geography> PlanarLine(const std::vector<R2Point>& positions,
                                     std::string_view failed);
absl::StatusOr<Geography> PlanarPolygon(const std::vector<std::vector<R2Point>>& rings,
                                        std::string_view failed, bool make_valid = false);

}  // namespace googlesql_geography
