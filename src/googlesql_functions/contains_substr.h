#pragma once

#include <optional>
#include <string_view>

#include "absl/status/statusor.h"

namespace googlesql {
class Value;
}

namespace googlesql_functions {

// BigQuery's recursive, NFKC case-folded substring search, which GoogleSQL does not provide.
absl::StatusOr<std::optional<bool>> ContainsSubstr(const googlesql::Value& value,
                                                   std::string_view search,
                                                   std::optional<std::string_view> json_scope);

}  // namespace googlesql_functions
