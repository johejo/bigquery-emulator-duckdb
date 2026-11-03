#pragma once

#include <string>
#include <string_view>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "src/field_schema.h"
#include "src/file_read_options.h"

namespace bigquery_emulator_duckdb {

// The text of the CSV or JSON value `text` of a `type` field that the reader converts, as
// BigQuery's external reads take it, or BigQuery's message for a value they reject. As BigQuery's
// answers in external_table_csv_types.api.answers.json and
// external_table_json_types.api.answers.json show:
// - DATETIME takes YYYY-[M]M-[D]D, optionally followed by a space or T and [H]H:[M]M:[S]S with at
//   most six fractional digits, without surrounding whitespace, as canonical text.
// - BIGNUMERIC takes a decimal number with at most 38 fractional digits, after surrounding
//   whitespace, as canonical text, but not NaN or a hex or grouped number.
// - JSON takes a JSON document, as compact text with the first of duplicate keys, and rejects one
//   with nlohmann's message, which BigQuery's is.
// - GEOGRAPHY takes WKT, hex WKB or GeoJSON, which the reader converts, and WKT with the prefix
//   SRID=4326;, without it.
// With `formats`, as BigQuery's answers in external_table_datetime_formats.api.answers.json show,
// a DATE, DATETIME, TIME or TIMESTAMP takes only its format, as CAST ... FORMAT parses it, and a
// TIMESTAMP without a format but with a time zone the date, a space or T, the time, and an
// optional UTC offset, Z or UTC of the data types of
// third_party/bigquery_docs/loading-data-cloud-storage-csv.md, in that time zone without one.
// Other types take `text` as it is. Returns ApiError::Unsupported's status, of code
// kUnimplemented, for a form the emulator does not read.
absl::StatusOr<std::string> ExternalValue(FieldType type, std::string_view text,
                                          const DatetimeFormats& formats = {});

// BigQuery's error, of reason invalidQuery, for `formats` it does not take, as its answers in
// external_table_datetime_formats.api.answers.json show: a time_zone other than a time zone name,
// or a format that CAST ... FORMAT cannot parse its type with.
absl::Status CheckDatetimeFormats(const DatetimeFormats& formats);

}  // namespace bigquery_emulator_duckdb
