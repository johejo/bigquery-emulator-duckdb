#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "absl/status/statusor.h"
#include "googlesql/public/numeric_value.h"

namespace googlesql_functions {

absl::StatusOr<std::string> CastBytesToUtf8(std::string_view bytes);

absl::StatusOr<int64_t> CastFloat64ToInt64(double value);

absl::StatusOr<bool> CastStringToBool(std::string_view text);

absl::StatusOr<std::string> CastBytesToString(std::string_view bytes, std::string_view format);

absl::StatusOr<std::string> CastDateToString(int32_t date, std::string_view format);

absl::StatusOr<std::string> CastDatetimeToString(int64_t micros, std::string_view format);

absl::StatusOr<std::string> CastTimeToString(int64_t micros, std::string_view format);

absl::StatusOr<std::string> CastTimestampToString(int64_t micros, std::string_view format,
                                                  std::string_view time_zone);

absl::StatusOr<std::string> CastInt64ToString(int64_t value, std::string_view format);

absl::StatusOr<std::string> CastFloat64ToString(double value, std::string_view format);

absl::StatusOr<std::string> CastNumericToString(googlesql::NumericValue value,
                                                std::string_view format);

absl::StatusOr<std::string> CastBigNumericToString(googlesql::BigNumericValue value,
                                                   std::string_view format);

absl::StatusOr<std::string> CastStringToBytes(std::string_view text, std::string_view format);

absl::StatusOr<int32_t> CastStringToDate(std::string_view text, std::string_view format,
                                         int64_t now);

absl::StatusOr<int64_t> CastStringToDatetime(std::string_view text, std::string_view format,
                                             int64_t now);

absl::StatusOr<int64_t> CastStringToTime(std::string_view text, std::string_view format);

absl::StatusOr<int64_t> CastStringToTimestamp(std::string_view text, std::string_view format,
                                              std::string_view time_zone, int64_t now);

}  // namespace googlesql_functions
