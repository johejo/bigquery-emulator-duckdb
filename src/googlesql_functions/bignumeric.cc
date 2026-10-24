#include "src/googlesql_functions/bignumeric.h"

#include <format>
#include <string>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "googlesql/public/numeric_value.h"

namespace googlesql_functions {
using googlesql::BigNumericValue;

absl::StatusOr<BigNumericValue> PrecisionScale::Apply(BigNumericValue value) const {
  const auto rounded = value.Round(scale);
  if (!rounded.ok()) {
    return rounded.status();
  }
  std::string nines(precision, '9');
  if (scale > 0) {
    nines.insert(nines.size() - scale, ".");
  }
  const auto upper = BigNumericValue::FromString(nines);
  if (!upper.ok()) {
    return upper.status();
  }
  if (*rounded <= *upper && *rounded >= upper->Negate().value()) {
    return *rounded;
  }
  const std::string type = scale == 0 ? std::format("BIGNUMERIC({})", precision)
                                      : std::format("BIGNUMERIC({}, {})", precision, scale);
  return absl::OutOfRangeError(std::format(
      "{} has precision {} and scale {} but got a value that is not in range of [-{}, {}]", type,
      precision, scale, upper->ToString(), upper->ToString()));
}

}  // namespace googlesql_functions
