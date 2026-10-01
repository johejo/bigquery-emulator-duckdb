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
  std::string name;
  FieldType type = FieldType::kString;
  FieldMode mode = FieldMode::kNullable;
  std::vector<FieldSchema> fields;  // Populated for RECORD.
  std::string description;
  std::optional<int64_t> max_length;  // STRING and BYTES.
  std::optional<int64_t> precision;   // NUMERIC and BIGNUMERIC.
  std::optional<int64_t> scale;       // NUMERIC and BIGNUMERIC.
  std::string default_value_expression;
  std::vector<std::string> policy_tags;  // policyTags.names.

  nlohmann::json ToJson() const;
};

// Parses a TableFieldSchema object. Throws ApiError::Invalid for a field BigQuery would reject.
FieldSchema FieldSchemaFromJson(const nlohmann::json& value);

// A TableSchema object: {"fields": [...]}.
nlohmann::json SchemaToJson(const std::vector<FieldSchema>& schema);
std::vector<FieldSchema> SchemaFromJson(const nlohmann::json& schema);

}  // namespace bigquery_emulator_duckdb
