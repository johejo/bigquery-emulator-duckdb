#include "src/field_schema.h"

#include <array>
#include <charconv>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "nlohmann/json.hpp"
#include "src/api_error.h"

namespace bigquery_emulator_duckdb {
namespace {

using nlohmann::json;

struct TypeName {
  std::string_view name;
  FieldType type;
};

// The reported name of each type comes first.
constexpr std::array kTypeNames = {
    TypeName{"STRING", FieldType::kString},       TypeName{"BYTES", FieldType::kBytes},
    TypeName{"INTEGER", FieldType::kInteger},     TypeName{"INT64", FieldType::kInteger},
    TypeName{"FLOAT", FieldType::kFloat},         TypeName{"FLOAT64", FieldType::kFloat},
    TypeName{"NUMERIC", FieldType::kNumeric},     TypeName{"BIGNUMERIC", FieldType::kBigNumeric},
    TypeName{"BOOLEAN", FieldType::kBoolean},     TypeName{"BOOL", FieldType::kBoolean},
    TypeName{"TIMESTAMP", FieldType::kTimestamp}, TypeName{"DATE", FieldType::kDate},
    TypeName{"TIME", FieldType::kTime},           TypeName{"DATETIME", FieldType::kDatetime},
    TypeName{"INTERVAL", FieldType::kInterval},   TypeName{"GEOGRAPHY", FieldType::kGeography},
    TypeName{"JSON", FieldType::kJson},           TypeName{"RECORD", FieldType::kRecord},
    TypeName{"STRUCT", FieldType::kRecord},
};

struct ModeName {
  std::string_view name;
  FieldMode mode;
};

constexpr std::array kModeNames = {
    ModeName{"NULLABLE", FieldMode::kNullable},
    ModeName{"REQUIRED", FieldMode::kRequired},
    ModeName{"REPEATED", FieldMode::kRepeated},
};

bool EqualsIgnoringCase(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) {
    return false;
  }
  for (size_t i = 0; i < a.size(); ++i) {
    const auto upper = [](char c) { return c >= 'a' && c <= 'z' ? static_cast<char>(c - 32) : c; };
    if (upper(a[i]) != upper(b[i])) {
      return false;
    }
  }
  return true;
}

std::string StringMember(const json& value, const char* key) {
  const auto it = value.find(key);
  if (it == value.end() || it->is_null()) {
    return "";
  }
  if (!it->is_string()) {
    throw ApiError::Invalid(std::string("Field ") + key + " must be a string");
  }
  return it->get<std::string>();
}

// An int64 member, which BigQuery encodes as a decimal string; a JSON number is accepted too.
std::optional<int64_t> Int64Member(const json& value, const char* key) {
  const auto it = value.find(key);
  if (it == value.end() || it->is_null()) {
    return std::nullopt;
  }
  if (it->is_number_integer()) {
    return it->get<int64_t>();
  }
  if (it->is_string()) {
    const auto& text = it->get_ref<const std::string&>();
    int64_t parsed = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), parsed);
    if (error == std::errc() && end == text.data() + text.size()) {
      return parsed;
    }
  }
  throw ApiError::Invalid(std::string("Field ") + key + " must be an integer");
}

}  // namespace

std::optional<FieldType> ParseFieldType(std::string_view name) {
  for (const TypeName& entry : kTypeNames) {
    if (EqualsIgnoringCase(entry.name, name)) {
      return entry.type;
    }
  }
  return std::nullopt;
}

std::string_view FieldTypeName(FieldType type) {
  for (const TypeName& entry : kTypeNames) {
    if (entry.type == type) {
      return entry.name;
    }
  }
  return "";
}

std::optional<FieldMode> ParseFieldMode(std::string_view name) {
  for (const ModeName& entry : kModeNames) {
    if (EqualsIgnoringCase(entry.name, name)) {
      return entry.mode;
    }
  }
  return std::nullopt;
}

std::string_view FieldModeName(FieldMode mode) {
  for (const ModeName& entry : kModeNames) {
    if (entry.mode == mode) {
      return entry.name;
    }
  }
  return "";
}

json FieldSchema::ToJson() const {
  json field = {{"name", name}, {"type", FieldTypeName(type)}, {"mode", FieldModeName(mode)}};
  if (!fields.empty()) {
    field["fields"] = json::array();
    for (const FieldSchema& child : fields) {
      field["fields"].push_back(child.ToJson());
    }
  }
  if (!description.empty()) {
    field["description"] = description;
  }
  if (max_length.has_value()) {
    field["maxLength"] = std::to_string(*max_length);
  }
  if (precision.has_value()) {
    field["precision"] = std::to_string(*precision);
  }
  if (scale.has_value()) {
    field["scale"] = std::to_string(*scale);
  }
  if (!default_value_expression.empty()) {
    field["defaultValueExpression"] = default_value_expression;
  }
  if (!policy_tags.empty()) {
    field["policyTags"] = {{"names", policy_tags}};
  }
  return field;
}

FieldSchema FieldSchemaFromJson(const json& value) {
  if (!value.is_object()) {
    throw ApiError::Invalid("A schema field must be an object");
  }
  FieldSchema field;
  field.name = StringMember(value, "name");
  if (field.name.empty()) {
    throw ApiError::Invalid("A schema field is missing a name");
  }
  if (const std::string type = StringMember(value, "type"); !type.empty()) {
    const std::optional<FieldType> parsed = ParseFieldType(type);
    if (!parsed.has_value()) {
      throw ApiError::Invalid("Unsupported field type: " + type);
    }
    field.type = *parsed;
  }
  if (const std::string mode = StringMember(value, "mode"); !mode.empty()) {
    const std::optional<FieldMode> parsed = ParseFieldMode(mode);
    if (!parsed.has_value()) {
      throw ApiError::Invalid("Unsupported field mode: " + mode);
    }
    field.mode = *parsed;
  }
  if (const auto it = value.find("fields"); it != value.end() && !it->is_null()) {
    if (!it->is_array()) {
      throw ApiError::Invalid("Field fields must be an array");
    }
    for (const json& child : *it) {
      field.fields.push_back(FieldSchemaFromJson(child));
    }
  }
  field.description = StringMember(value, "description");
  field.max_length = Int64Member(value, "maxLength");
  field.precision = Int64Member(value, "precision");
  field.scale = Int64Member(value, "scale");
  field.default_value_expression = StringMember(value, "defaultValueExpression");
  if (const auto tags = value.find("policyTags"); tags != value.end() && tags->is_object()) {
    for (const json& name : tags->value("names", json::array())) {
      if (!name.is_string()) {
        throw ApiError::Invalid("Field policyTags.names must hold strings");
      }
      field.policy_tags.push_back(name.get<std::string>());
    }
  }
  return field;
}

json SchemaToJson(const std::vector<FieldSchema>& schema) {
  json fields = json::array();
  for (const FieldSchema& field : schema) {
    fields.push_back(field.ToJson());
  }
  return json{{"fields", std::move(fields)}};
}

std::vector<FieldSchema> SchemaFromJson(const json& schema) {
  std::vector<FieldSchema> fields;
  if (!schema.is_object()) {
    return fields;
  }
  const auto it = schema.find("fields");
  if (it == schema.end() || it->is_null()) {
    return fields;
  }
  if (!it->is_array()) {
    throw ApiError::Invalid("Schema fields must be an array");
  }
  for (const json& field : *it) {
    fields.push_back(FieldSchemaFromJson(field));
  }
  return fields;
}

}  // namespace bigquery_emulator_duckdb
