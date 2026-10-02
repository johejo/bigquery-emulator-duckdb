#include "src/table_metadata.h"

#include <cstddef>
#include <format>
#include <map>
#include <string>

#include "nlohmann/json.hpp"
#include "src/api_error.h"

namespace bigquery_emulator_duckdb {

using nlohmann::json;

namespace {

constexpr size_t kMaxLabels = 64;
constexpr size_t kMaxLabelLength = 63;

// A string member, or "" when `value` is null.
std::string StringValue(const json& value, const char* key) {
  if (value.is_null()) {
    return "";
  }
  if (!value.is_string()) {
    throw ApiError::Invalid(std::string("Field ") + key + " must be a string");
  }
  return value.get<std::string>();
}

// Applies the labels of `labels`, a JSON object, to `into`; a null value removes a label.
void MergeLabels(const json& labels, std::map<std::string, std::string>& into) {
  if (labels.is_null()) {
    return;
  }
  if (!labels.is_object()) {
    throw ApiError::Invalid("Field labels must be an object");
  }
  for (const auto& [key, value] : labels.items()) {
    if (value.is_null()) {
      into.erase(key);
    } else {
      into[key] = StringValue(value, "labels");
    }
  }
}

// The characters of UTF-8 `text`: its bytes other than continuation bytes.
size_t CharacterCount(const std::string& text) {
  size_t count = 0;
  for (const char c : text) {
    count += (static_cast<unsigned char>(c) & 0xC0) != 0x80 ? 1 : 0;
  }
  return count;
}

bool LabelCharacter(char c) {
  return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-' ||
         static_cast<unsigned char>(c) >= 0x80;
}

// What is wrong with a label key or value `text`, or "" when nothing is.
std::string LabelTextProblem(const std::string& text) {
  if (CharacterCount(text) > kMaxLabelLength) {
    return "is longer than 63 characters";
  }
  for (const char c : text) {
    if (!LabelCharacter(c)) {
      return "can contain only lowercase letters, numeric characters, underscores and dashes";
    }
  }
  return "";
}

}  // namespace

json TableMetadata::ToJson() const {
  json fields = json::object();
  if (!description.empty()) {
    fields["description"] = description;
  }
  if (!friendly_name.empty()) {
    fields["friendlyName"] = friendly_name;
  }
  if (!labels.empty()) {
    fields["labels"] = labels;
  }
  return fields;
}

TableMetadata TableMetadataFromJson(const json& table) {
  TableMetadata metadata;
  PatchTableMetadata(metadata, table);
  return metadata;
}

void PatchTableMetadata(TableMetadata& metadata, const json& patch) {
  if (const auto it = patch.find("description"); it != patch.end()) {
    metadata.description = StringValue(*it, "description");
  }
  if (const auto it = patch.find("friendlyName"); it != patch.end()) {
    metadata.friendly_name = StringValue(*it, "friendlyName");
  }
  if (const auto it = patch.find("labels"); it != patch.end()) {
    MergeLabels(*it, metadata.labels);
  }
  ValidateLabels(metadata.labels);
}

void ValidateLabels(const std::map<std::string, std::string>& labels) {
  if (labels.size() > kMaxLabels) {
    throw ApiError::Invalid("A resource can have at most 64 labels");
  }
  for (const auto& [key, value] : labels) {
    const std::string what = "Label key \"" + key + "\"";
    if (key.empty() ||
        !((key[0] >= 'a' && key[0] <= 'z') || static_cast<unsigned char>(key[0]) >= 0x80)) {
      throw ApiError::Invalid(what + " must start with a lowercase letter");
    }
    if (const std::string problem = LabelTextProblem(key); !problem.empty()) {
      throw ApiError::Invalid(std::format("{} {}", what, problem));
    }
    if (const std::string problem = LabelTextProblem(value); !problem.empty()) {
      throw ApiError::Invalid(std::format("Label value \"{}\" {}", value, problem));
    }
  }
}

}  // namespace bigquery_emulator_duckdb
