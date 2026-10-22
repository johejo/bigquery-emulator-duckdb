#include "src/field_schema.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <map>
#include <optional>
#include <set>
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
    TypeName{.name = "STRING", .type = FieldType::kString},
    TypeName{.name = "BYTES", .type = FieldType::kBytes},
    TypeName{.name = "INTEGER", .type = FieldType::kInteger},
    TypeName{.name = "INT64", .type = FieldType::kInteger},
    TypeName{.name = "FLOAT", .type = FieldType::kFloat},
    TypeName{.name = "FLOAT64", .type = FieldType::kFloat},
    TypeName{.name = "NUMERIC", .type = FieldType::kNumeric},
    TypeName{.name = "BIGNUMERIC", .type = FieldType::kBigNumeric},
    TypeName{.name = "BOOLEAN", .type = FieldType::kBoolean},
    TypeName{.name = "BOOL", .type = FieldType::kBoolean},
    TypeName{.name = "TIMESTAMP", .type = FieldType::kTimestamp},
    TypeName{.name = "DATE", .type = FieldType::kDate},
    TypeName{.name = "TIME", .type = FieldType::kTime},
    TypeName{.name = "DATETIME", .type = FieldType::kDatetime},
    TypeName{.name = "INTERVAL", .type = FieldType::kInterval},
    TypeName{.name = "RANGE", .type = FieldType::kRange},
    TypeName{.name = "GEOGRAPHY", .type = FieldType::kGeography},
    TypeName{.name = "JSON", .type = FieldType::kJson},
    TypeName{.name = "RECORD", .type = FieldType::kRecord},
    TypeName{.name = "STRUCT", .type = FieldType::kRecord},
};

struct ModeName {
  std::string_view name;
  FieldMode mode;
};

constexpr std::array kModeNames = {
    ModeName{.name = "NULLABLE", .mode = FieldMode::kNullable},
    ModeName{.name = "REQUIRED", .mode = FieldMode::kRequired},
    ModeName{.name = "REPEATED", .mode = FieldMode::kRepeated},
};

bool EqualsIgnoringCase(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) {
    return false;
  }
  for (size_t i = 0; i < a.size(); ++i) {
    const auto upper = [](char c) { return c >= 'a' && c <= 'z' ? static_cast<char>(c - 32) : c; };
    if (upper(a.at(i)) != upper(b.at(i))) {
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

// Indexes refer to the original schema, so projecting keeps its order and spelling.
struct SchemaSelection {
  bool all = false;
  std::map<size_t, SchemaSelection> fields = {};
};

void SelectSchemaField(const std::vector<FieldSchema>& schema, std::string_view path,
                       SchemaSelection& selection) {
  const auto dot = path.find('.');
  const std::string_view name = path.substr(0, dot);
  const auto field = std::ranges::find_if(schema, [name](const FieldSchema& candidate) {
    return std::ranges::equal(candidate.name, name, [](unsigned char a, unsigned char b) {
      return std::tolower(a) == std::tolower(b);
    });
  });
  if (name.empty() || field == schema.end()) {
    throw ApiError::Invalid("Unknown field in selectedFields: " + std::string(path));
  }
  auto& child = selection.fields[static_cast<size_t>(field - schema.begin())];
  if (dot == std::string_view::npos) {
    child.all = true;
  } else {
    if (field->type != FieldType::kRecord) {
      throw ApiError::Invalid("Not a RECORD field in selectedFields: " + std::string(name));
    }
    SelectSchemaField(field->fields, path.substr(dot + 1), child);
  }
}

SchemaSelection ParseSchemaSelection(const std::vector<FieldSchema>& schema,
                                     std::string_view remaining) {
  SchemaSelection selection;
  while (true) {
    const auto comma = remaining.find(',');
    SelectSchemaField(schema, remaining.substr(0, comma), selection);
    if (comma == std::string_view::npos) {
      break;
    }
    remaining.remove_prefix(comma + 1);
  }
  return selection;
}

std::vector<FieldSchema> ProjectSchema(const std::vector<FieldSchema>& schema,
                                       const SchemaSelection& selection) {
  std::vector<FieldSchema> projected;
  for (const auto& [index, child] : selection.fields) {
    FieldSchema field = schema.at(index);
    if (!child.all) {
      field.fields = ProjectSchema(field.fields, child);
    }
    projected.push_back(std::move(field));
  }
  return projected;
}

}  // namespace

std::optional<FieldType> ParseFieldType(std::string_view name) {
  const auto* it = std::ranges::find_if(
      kTypeNames, [name](const TypeName& entry) { return EqualsIgnoringCase(entry.name, name); });
  if (it != kTypeNames.end()) {
    return it->type;
  }
  return std::nullopt;
}

std::string_view FieldTypeName(FieldType type) {
  const auto* it = std::ranges::find(kTypeNames, type, &TypeName::type);
  if (it != kTypeNames.end()) {
    return it->name;
  }
  return "";
}

std::optional<FieldMode> ParseFieldMode(std::string_view name) {
  const auto* it = std::ranges::find_if(
      kModeNames, [name](const ModeName& entry) { return EqualsIgnoringCase(entry.name, name); });
  if (it != kModeNames.end()) {
    return it->mode;
  }
  return std::nullopt;
}

std::string_view FieldModeName(FieldMode mode) {
  const auto* it = std::ranges::find(kModeNames, mode, &ModeName::mode);
  if (it != kModeNames.end()) {
    return it->name;
  }
  return "";
}

json FieldSchema::ToJson() const {
  json field = {{"name", name}, {"type", FieldTypeName(type)}};
  if (mode_given || mode != FieldMode::kNullable) {
    field["mode"] = FieldModeName(mode);
  }
  if (range_element_type.has_value()) {
    field["rangeElementType"] = {{"type", FieldTypeName(*range_element_type)}};
  }
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
  if (!rounding_mode.empty()) {
    field["roundingMode"] = rounding_mode;
  }
  if (!policy_tags.empty()) {
    field["policyTags"] = {{"names", policy_tags}};
  }
  return field;
}

FieldSchema RangeBoundField(const FieldSchema& field, std::string name) {
  if (!field.range_element_type) {
    throw ApiError::Internal("RANGE field " + field.name + " has no element type");
  }
  return {.name = std::move(name), .type = *field.range_element_type};
}

std::optional<std::array<std::optional<std::string>, 2>> RangeCellBounds(std::string_view text) {
  const size_t separator = text.find(", ");
  if (text.size() < 6 || text.front() != '[' || text.back() != ')' ||
      separator == std::string_view::npos) {
    return std::nullopt;
  }
  const auto bound = [](std::string_view cell) {
    return cell == "UNBOUNDED" ? std::nullopt : std::optional<std::string>(cell);
  };
  return std::array{
      bound(text.substr(1, separator - 1)),
      bound(text.substr(separator + 2, text.size() - separator - 3)),
  };
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
  const std::string type = StringMember(value, "type");
  if (type.empty()) {
    throw ApiError::Invalid("Field " + field.name + " missing type");
  }
  const std::optional<FieldType> parsed_type = ParseFieldType(type);
  if (!parsed_type.has_value()) {
    throw ApiError::Invalid("Unsupported field type: " + type);
  }
  field.type = *parsed_type;
  if (const std::string mode = StringMember(value, "mode"); !mode.empty()) {
    const std::optional<FieldMode> parsed = ParseFieldMode(mode);
    if (!parsed.has_value()) {
      throw ApiError::Invalid("Invalid value for mode: " + mode + " is not a valid value");
    }
    field.mode = *parsed;
    field.mode_given = true;
  }
  if (field.type == FieldType::kRange) {
    const auto it = value.find("rangeElementType");
    const std::optional<FieldType> element = it == value.end() || !it->is_object()
                                                 ? std::nullopt
                                                 : ParseFieldType(StringMember(*it, "type"));
    if (element != FieldType::kDate && element != FieldType::kDatetime &&
        element != FieldType::kTimestamp) {
      throw ApiError::Invalid("Field " + field.name +
                              " of type RANGE needs a rangeElementType of DATE, DATETIME or "
                              "TIMESTAMP");
    }
    field.range_element_type = element;
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
  const std::string mode = StringMember(value, "roundingMode");
  std::optional<std::string> parsed = ParseRoundingMode(mode);
  if (!parsed.has_value()) {
    throw ApiError::Invalid("Invalid rounding mode for field " + field.name + ": " + mode);
  }
  field.rounding_mode = *std::move(parsed);
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

std::optional<std::string> ParseRoundingMode(const std::string& mode) {
  if (mode == "ROUNDING_MODE_UNSPECIFIED") {
    return "";
  }
  if (!mode.empty() && mode != kRoundHalfAwayFromZero && mode != kRoundHalfEven) {
    return std::nullopt;
  }
  return mode;
}

void ApplyDefaultRoundingMode(FieldSchema& field, std::string_view mode) {
  if (field.rounding_mode.empty() &&
      (field.type == FieldType::kNumeric || field.type == FieldType::kBigNumeric)) {
    field.rounding_mode = mode;
  }
  ApplyDefaultRoundingMode(field.fields, mode);
}

void ApplyDefaultRoundingMode(std::vector<FieldSchema>& schema, std::string_view mode) {
  for (FieldSchema& field : schema) {
    ApplyDefaultRoundingMode(field, mode);
  }
}

void ApplyDefaultRoundingMode(const std::vector<FieldSchema>& current,
                              std::vector<FieldSchema>& updated, std::string_view mode) {
  for (size_t i = 0; i < updated.size(); ++i) {
    if (i >= current.size()) {
      ApplyDefaultRoundingMode(updated.at(i), mode);
    } else if (updated.at(i).name == current.at(i).name) {
      ApplyDefaultRoundingMode(current.at(i).fields, updated.at(i).fields, mode);
    }
  }
}

bool HasRoundHalfEven(const std::vector<FieldSchema>& schema) {
  return std::ranges::any_of(schema, [](const FieldSchema& field) {
    return field.rounding_mode == kRoundHalfEven || HasRoundHalfEven(field.fields);
  });
}

bool RoundsToEven(const FieldSchema& field) {
  return (field.rounding_mode == kRoundHalfEven && field.precision.has_value()) ||
         std::ranges::any_of(field.fields, RoundsToEven);
}

FieldSchema WithoutRoundingParameters(FieldSchema field) {
  if (field.rounding_mode == kRoundHalfEven) {
    field.precision.reset();
    field.scale.reset();
  }
  for (FieldSchema& child : field.fields) {
    child = WithoutRoundingParameters(std::move(child));
  }
  return field;
}

void CheckRoundingModes(const std::vector<FieldSchema>& schema) {
  for (const FieldSchema& field : schema) {
    if (!field.rounding_mode.empty() && field.type != FieldType::kNumeric &&
        field.type != FieldType::kBigNumeric) {
      throw ApiError::Unsupported("rounding modes on " + std::string(FieldTypeName(field.type)) +
                                  " fields");
    }
    CheckRoundingModes(field.fields);
  }
}

std::optional<std::string> DuplicateFieldPath(const std::vector<FieldSchema>& schema,
                                              const std::string& path) {
  std::set<std::string> names;
  for (const FieldSchema& field : schema) {
    std::string field_path = path.empty() ? field.name : path + "." + field.name;
    std::string name = field.name;
    std::ranges::transform(name, name.begin(), [](char c) {
      return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
    });
    if (!names.insert(std::move(name)).second) {
      return field_path;
    }
    if (std::optional<std::string> duplicate = DuplicateFieldPath(field.fields, field_path)) {
      return duplicate;
    }
  }
  return std::nullopt;
}

json SchemaToJson(const std::vector<FieldSchema>& schema) {
  json fields = json::array();
  for (const FieldSchema& field : schema) {
    fields.push_back(field.ToJson());
  }
  return json{{"fields", std::move(fields)}};
}

void SetModesGiven(std::vector<FieldSchema>& schema) {
  for (FieldSchema& field : schema) {
    field.mode_given = true;
    SetModesGiven(field.fields);
  }
}

bool SameSchema(std::vector<FieldSchema> a, std::vector<FieldSchema> b) {
  SetModesGiven(a);
  SetModesGiven(b);
  return SchemaToJson(a) == SchemaToJson(b);
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
  std::ranges::transform(*it, std::back_inserter(fields), FieldSchemaFromJson);
  if (const std::optional<std::string> duplicate = DuplicateFieldPath(fields)) {
    throw ApiError::Invalid("Field " + *duplicate + " already exists in schema");
  }
  return fields;
}

std::vector<FieldSchema> SelectFields(const std::vector<FieldSchema>& schema,
                                      std::string_view selected_fields) {
  return ProjectSchema(schema, ParseSchemaSelection(schema, selected_fields));
}

}  // namespace bigquery_emulator_duckdb
