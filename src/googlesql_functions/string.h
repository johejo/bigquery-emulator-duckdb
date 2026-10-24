#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "absl/status/status.h"
#include "absl/status/statusor.h"

namespace googlesql_functions {

std::string ToBase32(std::string_view bytes);
absl::StatusOr<std::string> FromBase32(std::string_view text);
bool Chr(int64_t codepoint, std::string* out, absl::Status* error);
absl::StatusOr<std::string> DecimalString(std::string_view text);
bool DoubleString(double value, std::string* out, absl::Status* error);
absl::StatusOr<bool> CheckStringLength(std::string_view text, int64_t maximum,
                                       std::string_view column);
absl::StatusOr<bool> CheckBytesLength(std::string_view text, int64_t maximum,
                                      std::string_view column);
bool Normalize(std::string_view text, std::string_view mode, std::string* out, absl::Status* error);
bool NormalizeAndCasefold(std::string_view text, std::string_view mode, std::string* out,
                          absl::Status* error);

}  // namespace googlesql_functions
