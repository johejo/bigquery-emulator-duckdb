#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "absl/status/statusor.h"

namespace googlesql {
class Value;
}  // namespace googlesql

namespace googlesql_functions {

// Where FORMAT follows BigQuery rather than GoogleSQL.
struct FormatOptions {
  // Integer precision and grouping, and %t ending at NUL.
  bool adapt_specifiers = false;
  // No sign for a NaN in floating-point specifiers.
  bool unsigned_nan = false;
  // The sign of a floating-point negative zero.
  bool signed_zero = false;
};

absl::StatusOr<std::optional<std::string>> Format(std::string_view pattern,
                                                  const std::vector<googlesql::Value>& arguments,
                                                  const FormatOptions& options);

}  // namespace googlesql_functions
