#pragma once

#include <array>
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
  // Whether the mode was given, as in tables.insert or a load job; tables.get omits a NULLABLE mode
  // otherwise, as for a column of a DDL statement.
  bool mode_given = false;
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

// The field that the start or end bound of the RANGE `field` is, named `name`: a NULLABLE field of
// its element type, whose NULL is an unbounded end, as the Storage Read API gives a RANGE.
FieldSchema RangeBoundField(const FieldSchema& field, std::string name);

// The bounds of a RANGE cell, "[start, end)" as the emulator returns it, each a cell of the
// range's element type or nullopt where UNBOUNDED; nullopt for text of another form.
std::optional<std::array<std::optional<std::string>, 2>> RangeCellBounds(std::string_view text);

// The rounding modes a NUMERIC or BIGNUMERIC field takes.
inline constexpr std::string_view kRoundHalfAwayFromZero = "ROUND_HALF_AWAY_FROM_ZERO";
inline constexpr std::string_view kRoundHalfEven = "ROUND_HALF_EVEN";

// Whether a field of `schema`, at any depth, rounds halfway values to even. DuckDB's casts round
// them away from zero, so only writes that round values before the cast write such fields.
bool HasRoundHalfEven(const std::vector<FieldSchema>& schema);

// Whether values written to `field`, or to a field of it, are rounded to even to a scale, which
// DuckDB's casts to a parameterized NUMERIC or BIGNUMERIC round away from zero instead.
bool RoundsToEven(const FieldSchema& field);

// `field` without the precision and scale of the fields in it, at any depth, that round to even,
// so that a write reads their values unrounded and rounds them itself.
FieldSchema WithoutRoundingParameters(FieldSchema field);

// The rounding mode `mode` names, empty for one that leaves it unset ("" or
// ROUNDING_MODE_UNSPECIFIED), or nullopt for a value that names none.
std::optional<std::string> ParseRoundingMode(const std::string& mode);

// Gives the NUMERIC and BIGNUMERIC fields of `schema` that have no rounding mode, at any depth,
// `mode`, the default rounding mode of the table they are created in; an empty one leaves them
// unset. BigQuery records the mode on each field, so changing the default later keeps it.
void ApplyDefaultRoundingMode(std::vector<FieldSchema>& schema, std::string_view mode);
// The same for `field` and its fields.
void ApplyDefaultRoundingMode(FieldSchema& field, std::string_view mode);

// ApplyDefaultRoundingMode for the fields that `updated`, a schema update of `current`, adds at
// any depth; a schema update keeps existing fields in their positions.
void ApplyDefaultRoundingMode(const std::vector<FieldSchema>& current,
                              std::vector<FieldSchema>& updated, std::string_view mode);

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

// Marks the mode of each field of `schema`, at any depth, as given, as BigQuery reports it for a
// query result.
void SetModesGiven(std::vector<FieldSchema>& schema);

// The fields of `schema` that `selected_fields`, a comma-separated list of field paths such as
// a,r.x, names, in the schema's order and spelling, with only the named fields of a RECORD whose
// fields are named. Throws ApiError::Invalid for an unknown field or a path through a field that
// is not a RECORD.
std::vector<FieldSchema> SelectFields(const std::vector<FieldSchema>& schema,
                                      std::string_view selected_fields);

// Whether `a` and `b` are the same schema, whether or not their modes were given.
bool SameSchema(std::vector<FieldSchema> a, std::vector<FieldSchema> b);

}  // namespace bigquery_emulator_duckdb
