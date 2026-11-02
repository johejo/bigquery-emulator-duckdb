#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "src/field_schema.h"

namespace bigquery_emulator_duckdb {

// The types schema auto-detection gives values in CSV and JSON sources.
enum class DetectedKind : uint8_t { kString, kInteger, kFloat, kBoolean, kDate, kTime, kTimestamp };

// A value's detected type, and its text in a form FileReadQuery reads as that type.
struct DetectedValue {
  DetectedKind kind = DetectedKind::kString;
  std::string text;
};

// The type BigQuery detects for the text `input`, without leading and trailing spaces and tabs:
// a BOOL spelling other than 0 and 1, an integer, which may have a sign and thousands separators,
// a FLOAT64 as strtod reads it, a DATE, a TIME or a TIMESTAMP, or else a STRING, as an empty
// value is.
DetectedValue DetectValue(std::string_view input);

// The type of a column holding values of types `a` and `b`: INT64 and FLOAT64 make a FLOAT64, and
// other differing types a STRING.
DetectedKind MergeKinds(DetectedKind a, DetectedKind b);

FieldType KindType(DetectedKind kind);

// Throws ApiError::Invalid for a detected column name with a prefix BigQuery reserves.
void CheckReservedFieldName(const std::string& name);

}  // namespace bigquery_emulator_duckdb
