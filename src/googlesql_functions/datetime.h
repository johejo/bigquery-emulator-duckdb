#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "googlesql/public/civil_time.h"

namespace googlesql_functions {

// DATE uses days since the epoch. TIME, DATETIME and TIMESTAMP use microseconds;
// DATETIME encodes civil time as if it were UTC.
googlesql::TimeValue TimeFromMicros(int64_t micros);
int64_t MicrosFromTime(const googlesql::TimeValue& time);
absl::StatusOr<googlesql::DatetimeValue> DatetimeFromMicros(int64_t micros);
absl::StatusOr<int64_t> MicrosFromDatetime(const googlesql::DatetimeValue& datetime);

absl::Status FormatDate(std::string_view format, int32_t date, std::string* out);
absl::Status FormatDatetime(std::string_view format, int64_t micros, std::string* out);
absl::Status FormatTime(std::string_view format, int64_t micros, std::string* out);
absl::Status FormatTimestamp(std::string_view format, int64_t micros, std::string_view time_zone,
                             std::string* out);
absl::Status ParseDate(std::string_view format, std::string_view text, int32_t* out);
absl::StatusOr<int64_t> ParseDatetime(std::string_view format, std::string_view text);
absl::StatusOr<int64_t> ParseTime(std::string_view format, std::string_view text);
absl::Status ParseTimestamp(std::string_view format, std::string_view text,
                            std::string_view time_zone, int64_t* out);
absl::Status StringToDate(std::string_view text, int32_t* out);
absl::StatusOr<int64_t> StringToDatetime(std::string_view text);
absl::StatusOr<int64_t> StringToTime(std::string_view text);
absl::Status StringToTimestamp(std::string_view text, int64_t* out);
absl::Status StringToTimestampIn(std::string_view text, std::string_view time_zone, int64_t* out);
absl::Status TimestampToDatetimeIn(int64_t micros, std::string_view time_zone, int64_t* out);
absl::Status TimestampToTimeIn(int64_t micros, std::string_view time_zone, int64_t* out);
absl::Status TimestampCivilIn(int64_t micros, std::string_view time_zone, int64_t* out);
absl::Status DatetimeToTimestampIn(int64_t micros, std::string_view time_zone, int64_t* out);
absl::Status TimestampTrunc(int64_t micros, std::string_view part, std::string_view time_zone,
                            int64_t* out);
absl::Status GenerateDateArray(int32_t start, int32_t end, int64_t step, std::string_view part,
                               std::vector<int32_t>* out);
absl::Status DatetimeToString(int64_t micros, std::string* out);
absl::Status TimeToString(int64_t micros, std::string* out);
absl::Status TimestampToString(int64_t micros, std::string_view time_zone, std::string* out);

}  // namespace googlesql_functions
