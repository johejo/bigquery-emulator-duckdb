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

absl::StatusOr<std::optional<std::string>> Format(std::string_view pattern,
                                                  const std::vector<googlesql::Value>& values);

}  // namespace googlesql_functions
