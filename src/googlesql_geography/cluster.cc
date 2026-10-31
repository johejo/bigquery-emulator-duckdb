#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <optional>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "googlesql/base/status_macros.h"
#include "src/googlesql_geography/geography.h"
#include "src/googlesql_geography/value.h"

namespace googlesql_geography {

absl::StatusOr<std::vector<std::optional<int64_t>>> ClusterDBSCAN(
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): follows the SQL argument order.
    const std::vector<IndexedGeography>& geographies, double epsilon, int64_t minimum_geographies) {
  if (!(epsilon >= 0)) {
    return absl::InvalidArgumentError("Argument 2 to ST_CLUSTERDBSCAN must be at least 0");
  }
  if (minimum_geographies < 0) {
    return absl::InvalidArgumentError("Argument 3 to ST_CLUSTERDBSCAN must be at least 0");
  }
  const size_t size = geographies.size();
  const auto near = [&](size_t a, size_t b) {
    return DWithin(geographies.at(a), geographies.at(b), epsilon, false, "ST_CLUSTERDBSCAN");
  };
  // Count rows, not distinct shapes or their component points. Empty shapes never count,
  // even with a minimum of zero; every other shape is its own neighbor.
  std::vector<int64_t> counts(size);
  for (size_t i = 0; i < size; ++i) {
    if (!geographies.at(i).geography().Empty()) {
      ++counts.at(i);
    }
    for (size_t j = 0; j < i; ++j) {
      GOOGLESQL_ASSIGN_OR_RETURN(const bool adjacent, near(i, j));
      if (adjacent) {
        ++counts.at(i);
        ++counts.at(j);
      }
    }
  }
  std::vector<bool> core(size);
  for (size_t i = 0; i < size; ++i) {
    core.at(i) = !geographies.at(i).geography().Empty() && counts.at(i) >= minimum_geographies;
  }
  std::vector<size_t> parents(size);
  std::iota(parents.begin(), parents.end(), size_t{0});
  const auto root = [&](size_t i) {
    while (parents.at(i) != i) {
      parents.at(i) = parents.at(parents.at(i));
      i = parents.at(i);
    }
    return i;
  };
  // Only core-to-core links merge clusters. A border touching two clusters must not merge them.
  for (size_t i = 0; i < size; ++i) {
    if (!core.at(i)) {
      continue;
    }
    for (size_t j = 0; j < i; ++j) {
      if (!core.at(j)) {
        continue;
      }
      GOOGLESQL_ASSIGN_OR_RETURN(const bool adjacent, near(i, j));
      if (adjacent) {
        const size_t a = root(i);
        const size_t b = root(j);
        parents.at(std::max(a, b)) = std::min(a, b);
      }
    }
  }
  std::vector<std::optional<int64_t>> labels(size);
  int64_t next = 0;
  for (size_t i = 0; i < size; ++i) {
    if (core.at(i)) {
      const size_t representative = root(i);
      if (!labels.at(representative)) {
        labels.at(representative) = next++;
      }
      labels.at(i) = labels.at(representative);
    }
  }
  for (size_t i = 0; i < size; ++i) {
    if (core.at(i) || geographies.at(i).geography().Empty()) {
      continue;
    }
    // The first neighboring core gives a stable, valid assignment for ambiguous borders.
    for (size_t j = 0; j < size; ++j) {
      if (!core.at(j)) {
        continue;
      }
      GOOGLESQL_ASSIGN_OR_RETURN(const bool adjacent, near(i, j));
      if (adjacent) {
        labels.at(i) = labels.at(j);
        break;
      }
    }
  }
  return labels;
}

}  // namespace googlesql_geography
