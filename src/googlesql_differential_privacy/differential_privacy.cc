#include "src/googlesql_differential_privacy/differential_privacy.h"

#include <cstdint>
#include <memory>
#include <span>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
// The library's headers, templates included, do not compile under this repository's warnings.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#include "algorithms/approx-bounds.h"
#include "algorithms/bounded-mean.h"
#include "algorithms/bounded-sum.h"
#include "proto/data.pb.h"
#include "proto/util.h"
#pragma clang diagnostic pop

namespace googlesql_differential_privacy {
namespace {

// The algorithm's result over `values`, as the reference implementation evaluates
// $differential_privacy_sum and $differential_privacy_avg.
template <template <typename> typename Algorithm, typename T>
absl::StatusOr<T> Evaluate(std::span<const T> values, const Parameters<T>& parameters) {
  typename Algorithm<T>::Builder builder;
  builder.SetEpsilon(parameters.epsilon);
  // The library takes an int32; the analyzer keeps the option in its range.
  if (parameters.max_groups_contributed) {
    builder.SetMaxPartitionsContributed(static_cast<int32_t>(*parameters.max_groups_contributed));
  }
  if (parameters.bounds) {
    builder.SetLower(parameters.bounds->first).SetUpper(parameters.bounds->second);
  }
  absl::StatusOr<std::unique_ptr<Algorithm<T>>> algorithm = builder.Build();
  if (!algorithm.ok()) {
    return algorithm.status();
  }
  (*algorithm)->AddEntries(values.begin(), values.end());
  absl::StatusOr<differential_privacy::Output> output = (*algorithm)->PartialResult();
  if (!output.ok()) {
    if (output.status().GetPayload(differential_privacy::kApproxBoundsNotEnoughDataUrl)) {
      return T{};
    }
    return output.status();
  }
  return differential_privacy::GetValue<T>(*output);
}

}  // namespace

template <typename T>
absl::StatusOr<T> Sum(std::span<const T> values, const Parameters<T>& parameters) {
  return Evaluate<differential_privacy::BoundedSum>(values, parameters);
}

template absl::StatusOr<int64_t> Sum(std::span<const int64_t> values,
                                     const Parameters<int64_t>& parameters);
template absl::StatusOr<double> Sum(std::span<const double> values,
                                    const Parameters<double>& parameters);

absl::StatusOr<double> Mean(std::span<const double> values, const Parameters<double>& parameters) {
  return Evaluate<differential_privacy::BoundedMean>(values, parameters);
}

}  // namespace googlesql_differential_privacy
