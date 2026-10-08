#include "src/query_parameters.h"

#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "googlesql/public/numeric_value.h"
#include "nlohmann/json.hpp"
#include "src/api_error.h"
#include "src/bignumeric.h"
#include "src/duckdb_sql.h"
#include "src/field_schema.h"
#include "src/type_mapping.h"

namespace bigquery_emulator_duckdb {
namespace {

using nlohmann::json;

// BigQuery accepts the legacy TableFieldSchema spellings of the standard SQL type names in
// parameter types as well.
std::string CanonicalTypeName(const json& type) {
  if (!type.contains("type") || !type["type"].is_string()) {
    throw ApiError::Invalid("Query parameter type is missing a type name");
  }
  const std::string name = ToUpperAscii(type["type"].get<std::string>());
  static const auto* const kAliases = new std::unordered_map<std::string, std::string>{
      {"INTEGER", "INT64"}, {"FLOAT", "FLOAT64"}, {"BOOLEAN", "BOOL"}, {"RECORD", "STRUCT"}};
  const auto it = kAliases->find(name);
  return it == kAliases->end() ? name : it->second;
}

// A value together with the type that describes it: BigQuery keeps the two apart, in
// parameterType and parameterValue, at every level of a nested parameter.
struct TypedValue {
  const json& type;
  const json& value;
};

// The value of a parameter that carries none, which is how BigQuery spells a NULL.
const json& NoValue() {
  static const auto* const kNoValue = new json(json::object());
  return *kNoValue;
}

FieldSchema ToFieldSchema(const std::string& name, const json& type) {
  const std::string type_name = CanonicalTypeName(type);
  if (type_name == "ARRAY") {
    if (!type.contains("arrayType")) {
      throw ApiError::Invalid("ARRAY query parameter is missing arrayType");
    }
    FieldSchema field = ToFieldSchema(name, type["arrayType"]);
    if (field.mode == FieldMode::kRepeated) {
      throw ApiError::Invalid("ARRAY query parameter cannot contain an ARRAY");
    }
    field.mode = FieldMode::kRepeated;
    return field;
  }
  const std::optional<FieldType> field_type = ParseFieldType(type_name);
  if (!field_type.has_value()) {
    throw ApiError::Invalid("Unsupported query parameter type: " + type_name);
  }
  FieldSchema field{.name = name, .type = *field_type};
  if (field.type == FieldType::kRecord) {
    for (const json& child : type.value("structTypes", json::array())) {
      field.fields.push_back(ToFieldSchema(child.value("name", ""), child.at("type")));
    }
  }
  return field;
}

std::string ToDuckDbType(const json& type) {
  absl::StatusOr<std::string> duckdb_type = DuckDbColumnType(ToFieldSchema("", type));
  if (!duckdb_type.ok()) {
    throw ApiError::Invalid("Unsupported query parameter type: " +
                            std::string(duckdb_type.status().message()));
  }
  return *std::move(duckdb_type);
}

// The scalar text of a ParameterValue. BigQuery sends every scalar as a string; a client that
// sends a JSON number or boolean instead is accepted as well.
std::string ValueText(const json& value) {
  return value.is_string() ? value.get<std::string>() : value.dump();
}

std::string ToDuckDbLiteral(const TypedValue& parameter);

std::string ArrayLiteral(const TypedValue& parameter) {
  std::string elements;
  for (const json& element : parameter.value["arrayValues"]) {
    if (!elements.empty()) {
      elements += ", ";
    }
    elements += ToDuckDbLiteral({.type = parameter.type["arrayType"], .value = element});
  }
  // The cast pins down the element type, which an empty list literal does not carry.
  return "CAST([" + elements + "] AS " + ToDuckDbType(parameter.type) + ")";
}

std::string StructLiteral(const TypedValue& parameter) {
  // Parameter values are keyed by field name, so anonymous or duplicate names cannot carry
  // distinct field values. Validate their stored schema before spelling the value.
  (void)ToDuckDbType(parameter.type);
  const json& values = parameter.value["structValues"];
  std::string fields;
  for (const json& field : parameter.type.value("structTypes", json::array())) {
    if (!fields.empty()) {
      fields += ", ";
    }
    const std::string name = field.value("name", "");
    const auto value = values.find(name);
    fields += QuoteIdentifier(name) + " := " +
              ToDuckDbLiteral(
                  {.type = field.at("type"), .value = value == values.end() ? NoValue() : *value});
  }
  return "struct_pack(" + fields + ")";
}

std::string ToDuckDbLiteral(const TypedValue& parameter) {
  const json& type = parameter.type;
  const json& value = parameter.value;
  const std::string name = CanonicalTypeName(type);
  if (name == "ARRAY" && value.contains("arrayValues")) {
    return ArrayLiteral(parameter);
  }
  if (name == "STRUCT" && value.contains("structValues")) {
    return StructLiteral(parameter);
  }
  // A missing or null `value` is how BigQuery spells a NULL parameter; the cast keeps the type
  // of the NULL, which matters wherever the parameter is compared or inserted.
  if (!value.contains("value") || value["value"].is_null()) {
    return "CAST(NULL AS " + ToDuckDbType(type) + ")";
  }
  const std::string text = ValueText(value["value"]);
  if (ParseFieldType(name) == FieldType::kBigNumeric) {
    const auto number = googlesql::BigNumericValue::FromString(text);
    if (!number.ok()) {
      throw ApiError::Invalid("Invalid BIGNUMERIC query parameter value: " + text);
    }
    return BigNumericSql(*number);
  }
  if (name == "BYTES") {
    // BYTES parameters are base64 encoded, which DuckDB decodes back into a BLOB.
    return "from_base64(" + QuoteLiteral(text) + ")";
  }
  return "CAST(" + QuoteLiteral(text) + " AS " + ToDuckDbType(type) + ")";
}

}  // namespace

QueryParameters::QueryParameters(const std::vector<std::pair<std::string, std::string>>& named,
                                 std::vector<std::string> positional)
    : by_position_(std::move(positional)) {
  for (const auto& [name, literal] : named) {
    by_name_[ToUpperAscii(name)] = literal;
  }
}

QueryParameters QueryParameters::Parse(const json& parameters) {
  QueryParameters result;
  if (!parameters.is_array()) {
    throw ApiError::Invalid("queryParameters must be an array");
  }
  for (const json& parameter : parameters) {
    if (!parameter.contains("parameterType")) {
      throw ApiError::Invalid("Query parameter is missing parameterType");
    }
    const auto value = parameter.find("parameterValue");
    std::string literal = ToDuckDbLiteral({.type = parameter["parameterType"],
                                           .value = value == parameter.end() ? NoValue() : *value});
    const std::string name = parameter.value("name", "");
    FieldSchema type = ToFieldSchema(name, parameter["parameterType"]);
    if (name.empty()) {
      result.by_position_.push_back(std::move(literal));
      result.positional_types_.push_back(std::move(type));
    } else {
      result.by_name_[ToUpperAscii(name)] = std::move(literal);
      result.named_types_.push_back(std::move(type));
    }
  }
  return result;
}

const std::string& QueryParameters::ByName(const std::string& name) const {
  const auto it = by_name_.find(ToUpperAscii(name));
  if (it == by_name_.end()) {
    throw ApiError::InvalidQuery("Query parameter not found: @" + name);
  }
  return it->second;
}

const std::string& QueryParameters::ByPosition(int position) const {
  if (position < 1 || static_cast<size_t>(position) > by_position_.size()) {
    throw ApiError::InvalidQuery("Query parameter not found at position " +
                                 std::to_string(position));
  }
  return by_position_[position - 1];
}

}  // namespace bigquery_emulator_duckdb
