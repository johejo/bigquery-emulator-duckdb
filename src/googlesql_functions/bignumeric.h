#pragma once

#include <cstdint>

#include "absl/status/statusor.h"

namespace googlesql {
class BigNumericValue;
}  // namespace googlesql

namespace googlesql_functions {

// The precision and scale of a BIGNUMERIC(P, S) column, as GoogleSQL's reference implementation
// applies them: a value is rounded half away from zero to the scale, then must have no more
// digits than the precision.
struct PrecisionScale {
  int64_t precision;
  int64_t scale;

  [[nodiscard]] absl::StatusOr<googlesql::BigNumericValue> Apply(
      googlesql::BigNumericValue value) const;
};

}  // namespace googlesql_functions
