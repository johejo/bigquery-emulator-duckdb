#pragma once

#include <string>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/time/time.h"
#include "src/file_read_options.h"
#include "src/table_metadata.h"

namespace bigquery_emulator_duckdb {

// The time_zone, date_format, datetime_format, time_format and timestamp_format of `definition`.
DatetimeFormats ExternalFormats(const GcsExternalData& definition);

// The time zone `name`, a time_zone option, or UTC when it is empty, or the DDL's message for
// one that is not a time zone name.
absl::StatusOr<absl::TimeZone> FormatTimeZone(const std::string& name);

// BigQuery's message for `formats` it does not take, which DDL rejects with `ddl` and a query
// rejects, of a table tables.insert stored, otherwise, as its answers in
// external_table_datetime_formats.api.answers.json and
// external_table_types_followups.api.answers.json show: a time_zone other than a time zone name,
// or a format that CAST ... FORMAT cannot parse its type with.
absl::Status CheckDatetimeFormats(const DatetimeFormats& formats, bool ddl);

}  // namespace bigquery_emulator_duckdb
