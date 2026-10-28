#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <utility>

#include "absl/status/statusor.h"

namespace googlesql_differential_privacy {

// What a differentially private aggregate function takes from its query's privacy options.
template <typename T>
struct Parameters {
  // The function's share of the query's epsilon.
  double epsilon = 0;
  // Unset when the query lets a privacy unit contribute to any number of groups.
  std::optional<int64_t> max_groups_contributed;
  // The contribution bounds; unset bounds are computed from the values, with half the epsilon.
  std::optional<std::pair<T, T>> bounds;
};

// The noisy sum and mean of one contribution per privacy unit, with Google's differential privacy
// library, as GoogleSQL's reference implementation computes them. Without bounds, they are 0 when
// the values are too few to compute bounds from, as BigQuery answered, where the reference
// implementation returns NULL.
template <typename T>
absl::StatusOr<T> Sum(std::span<const T> values, const Parameters<T>& parameters);
absl::StatusOr<double> Mean(std::span<const double> values, const Parameters<double>& parameters);

}  // namespace googlesql_differential_privacy
