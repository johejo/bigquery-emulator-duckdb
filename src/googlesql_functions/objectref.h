#pragma once

#include <functional>
#include <string>

#include "absl/status/statusor.h"

namespace googlesql_functions {

absl::StatusOr<std::string> ObjectRefAuthorizer(const std::string& authorizer,
                                                const std::string& project);
absl::StatusOr<std::string> ReplaceObjectRefAuthorizer(
    const std::string& authorizer, const std::string& project,
    const std::function<bool(const std::string&)>& exists);
absl::StatusOr<std::string> ObjectRefDetails(std::string text);

}  // namespace googlesql_functions
