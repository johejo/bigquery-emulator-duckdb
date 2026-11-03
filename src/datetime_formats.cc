#include "src/datetime_formats.h"

#include <string>
#include <tuple>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/time/time.h"
#include "googlesql/public/functions/cast_date_time.h"
#include "googlesql/public/type.pb.h"
#include "src/file_read_options.h"
#include "src/table_metadata.h"

namespace bigquery_emulator_duckdb {

DatetimeFormats ExternalFormats(const GcsExternalData& definition) {
  return {
      .time_zone = definition.time_zone.value_or(""),
      .date = definition.date_format.value_or(""),
      .datetime = definition.datetime_format.value_or(""),
      .time = definition.time_format.value_or(""),
      .timestamp = definition.timestamp_format.value_or(""),
  };
}

absl::StatusOr<absl::TimeZone> FormatTimeZone(const std::string& name) {
  absl::TimeZone zone = absl::UTCTimeZone();
  if (!name.empty() && !absl::LoadTimeZone(name, &zone)) {
    return absl::InvalidArgumentError("time_zone " + name + " is not valid.");
  }
  return zone;
}

absl::Status CheckDatetimeFormats(const DatetimeFormats& formats, bool ddl) {
  if (const auto zone = FormatTimeZone(formats.time_zone); !zone.ok()) {
    return absl::InvalidArgumentError(ddl ? std::string(zone.status().message())
                                          : "Cannot set desired timezone: " + formats.time_zone +
                                                ".");
  }
  for (const auto& [name, format, kind] : {
           std::tuple{"date_format", &formats.date, googlesql::TYPE_DATE},
           {"datetime_format", &formats.datetime, googlesql::TYPE_DATETIME},
           {"time_format", &formats.time, googlesql::TYPE_TIME},
           {"timestamp_format", &formats.timestamp, googlesql::TYPE_TIMESTAMP},
       }) {
    if (format->empty()) {
      continue;
    }
    if (const absl::Status valid =
            googlesql::functions::ValidateFormatStringForParsing(*format, kind);
        !valid.ok()) {
      return ddl ? absl::InvalidArgumentError(
                       absl::StrCat("Invalid format for option '", name, "': ", valid.message()))
                 : valid;
    }
  }
  return absl::OkStatus();
}

}  // namespace bigquery_emulator_duckdb
