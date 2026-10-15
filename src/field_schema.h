#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "nlohmann/json_fwd.hpp"

namespace bigquery_emulator_duckdb {

// TableFieldSchema.type.
enum class FieldType : uint8_t {
  kString,
  kBytes,
  kInteger,
  kFloat,
  kNumeric,
  kBigNumeric,
  kBoolean,
  kTimestamp,
  kDate,
  kTime,
  kDatetime,
  kInterval,
  kRange,
  kGeography,
  kJson,
  kRecord,
};

// TableFieldSchema.mode.
enum class FieldMode : uint8_t { kNullable, kRequired, kRepeated };

// Parses a type name in any case. BigQuery accepts the standard SQL spellings (INT64, FLOAT64,
// BOOL, STRUCT) as well as the legacy ones it reports.
std::optional<FieldType> ParseFieldType(std::string_view name);
// The legacy name BigQuery reports for `type`: INTEGER, FLOAT, BOOLEAN, RECORD, ...
std::string_view FieldTypeName(FieldType type);

std::optional<FieldMode> ParseFieldMode(std::string_view name);
std::string_view FieldModeName(FieldMode mode);

// A column described with BigQuery's TableFieldSchema vocabulary.
struct FieldSchema {
  std::string name = {};
  FieldType type = FieldType::kString;
  FieldMode mode = FieldMode::kNullable;
  std::vector<FieldSchema> fields = {};  // Populated for RECORD.
  // rangeElementType.type for RANGE: DATE, DATETIME or TIMESTAMP.
  std::optional<FieldType> range_element_type = {};
  std::string description = {};
  std::optional<int64_t> max_length = {};  // STRING and BYTES.
  std::optional<int64_t> precision = {};   // NUMERIC and BIGNUMERIC.
  std::optional<int64_t> scale = {};       // NUMERIC and BIGNUMERIC.
  std::string default_value_expression = {};
  // roundingMode: ROUND_HALF_AWAY_FROM_ZERO or ROUND_HALF_EVEN, empty when unset.
  std::string rounding_mode = {};
  std::vector<std::string> policy_tags = {};  // policyTags.names.

  [[nodiscard]] nlohmann::json ToJson() const;
};

// `field` as NULLABLE, such as the element of a REPEATED field.
inline FieldSchema AsNullable(FieldSchema field) {
  field.mode = FieldMode::kNullable;
  return field;
}

// The rounding modes a NUMERIC or BIGNUMERIC field takes.
inline constexpr std::string_view kRoundHalfAwayFromZero = "ROUND_HALF_AWAY_FROM_ZERO";
inline constexpr std::string_view kRoundHalfEven = "ROUND_HALF_EVEN";

// Whether a field of `schema`, at any depth, rounds halfway values to even. DuckDB's casts round
// them away from zero, so only DML and CREATE TABLE AS SELECT, which round typed values before
// the cast, write such fields; loaded values can need rounding even without a precision.
bool HasRoundHalfEven(const std::vector<FieldSchema>& schema);

// Whether values written to `field`, or to a field of it, are rounded to even to a scale, which
// DuckDB's casts to a parameterized NUMERIC or BIGNUMERIC round away from zero instead.
bool RoundsToEven(const FieldSchema& field);

// Throws ApiError::Unsupported for a rounding mode the emulator cannot keep: one on a field that
// is not NUMERIC or BIGNUMERIC, or ROUND_HALF_EVEN on a parameterized field of a column with a
// default, whose value DuckDB would round away from zero.
void CheckRoundingModes(const std::vector<FieldSchema>& schema);

// The path, below `path`, of the first field of `schema` whose name another field beside it has,
// ignoring case as BigQuery does, at any depth; BigQuery rejects such a schema.
// https://cloud.google.com/bigquery/docs/schemas#column_names
std::optional<std::string> DuplicateFieldPath(const std::vector<FieldSchema>& schema,
                                              const std::string& path = "");

// Parses a TableFieldSchema object. Throws ApiError::Invalid for a field BigQuery would reject.
FieldSchema FieldSchemaFromJson(const nlohmann::json& value);

// A TableSchema object: {"fields": [...]}.
nlohmann::json SchemaToJson(const std::vector<FieldSchema>& schema);
std::vector<FieldSchema> SchemaFromJson(const nlohmann::json& schema);

}  // namespace bigquery_emulator_duckdb
