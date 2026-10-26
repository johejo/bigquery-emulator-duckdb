#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "absl/status/status.h"
#include "absl/status/statusor.h"

namespace googlesql_functions {

std::string ToBase32(std::string_view bytes);
absl::StatusOr<std::string> FromBase32(std::string_view text);
// CHR; `zero_is_empty` makes 0 an empty string, as in BigQuery, rather than GoogleSQL's NUL.
bool Chr(int64_t codepoint, bool zero_is_empty, std::string* out, absl::Status* error);
absl::StatusOr<std::string> DecimalString(std::string_view text);
bool DoubleString(double value, bool signed_zero, std::string* out, absl::Status* error);
absl::StatusOr<bool> CheckStringLength(std::string_view text, int64_t maximum,
                                       std::string_view column);
absl::StatusOr<bool> CheckBytesLength(std::string_view text, int64_t maximum,
                                      std::string_view column);
bool Normalize(std::string_view text, std::string_view mode, std::string* out, absl::Status* error);
bool NormalizeAndCasefold(std::string_view text, std::string_view mode, std::string* out,
                          absl::Status* error);

}  // namespace googlesql_functions
