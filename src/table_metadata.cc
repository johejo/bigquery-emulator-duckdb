#include "src/table_metadata.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <format>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "nlohmann/json.hpp"
#include "src/api_error.h"
#include "src/duckdb_sql.h"
#include "src/field_schema.h"

namespace bigquery_emulator_duckdb {

using nlohmann::json;

namespace {

constexpr size_t kMaxLabels = 64;
constexpr size_t kMaxLabelLength = 63;
constexpr size_t kMaxClusteringFields = 4;
constexpr int64_t kMaxRangePartitions = 10000;

ApiError Unsupported(std::string_view what) {
  return ApiError::Invalid(std::format("The emulator does not support {}", what));
}

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

// An int64 member of `value`, which BigQuery encodes as a decimal string; a JSON number is
// accepted too.
int64_t Int64Member(const json& value, const char* key) {
  const auto it = value.find(key);
  if (it != value.end() && it->is_number_integer()) {
    return it->get<int64_t>();
  }
  if (it != value.end() && it->is_string()) {
    const std::string& text = it->get_ref<const std::string&>();
    int64_t parsed = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), parsed);
    if (error == std::errc() && end == text.data() + text.size()) {
      return parsed;
    }
  }
  throw ApiError::Invalid(std::string("Field ") + key + " must be an integer");
}

std::optional<TimePartitioning> ParseTimePartitioning(const json& value) {
  if (value.is_null()) {
    return std::nullopt;
  }
  if (!value.is_object()) {
    throw ApiError::Invalid("Field timePartitioning must be an object");
  }
  TimePartitioning partitioning{.type = StringValue(value.value("type", json()), "type"),
                                .field = StringValue(value.value("field", json()), "field")};
  if (partitioning.type != "DAY" && partitioning.type != "HOUR" && partitioning.type != "MONTH" &&
      partitioning.type != "YEAR") {
    throw ApiError::Invalid("Invalid time partitioning type: " + partitioning.type);
  }
  if (partitioning.field.empty()) {
    throw Unsupported("ingestion-time partitioning");
  }
  if (!value.value("expirationMs", json()).is_null()) {
    throw Unsupported("partition expiration");
  }
  if (value.value("requirePartitionFilter", false) == true) {
    throw Unsupported("requirePartitionFilter");
  }
  return partitioning;
}

std::optional<RangePartitioning> ParseRangePartitioning(const json& value) {
  if (value.is_null()) {
    return std::nullopt;
  }
  if (!value.is_object() || !value.contains("range") || !value["range"].is_object()) {
    throw ApiError::Invalid("Field rangePartitioning must be an object with a range");
  }
  const json& range = value["range"];
  return RangePartitioning{.field = StringValue(value.value("field", json()), "field"),
                           .start = Int64Member(range, "start"),
                           .end = Int64Member(range, "end"),
                           .interval = Int64Member(range, "interval")};
}

std::vector<std::string> ParseClustering(const json& value) {
  if (value.is_null()) {
    return {};
  }
  if (!value.is_object() || !value.value("fields", json::array()).is_array()) {
    throw ApiError::Invalid("Field clustering must be an object with an array of fields");
  }
  std::vector<std::string> fields;
  for (const json& field : value.value("fields", json::array())) {
    fields.push_back(StringValue(field, "clustering.fields"));
  }
  return fields;
}

// The member `key` of `body`, or null when it has none.
json Member(const json& body, const char* key) { return body.value(key, json()); }

// Reads the partitioning of a Table resource `table` into `metadata`.
void ParsePartitioning(const json& table, TableMetadata& metadata) {
  metadata.time_partitioning = ParseTimePartitioning(Member(table, "timePartitioning"));
  metadata.range_partitioning = ParseRangePartitioning(Member(table, "rangePartitioning"));
  if (metadata.time_partitioning.has_value() && metadata.range_partitioning.has_value()) {
    throw ApiError::Invalid("A table cannot have both time and range partitioning");
  }
  if (Member(table, "requirePartitionFilter") == true) {
    throw Unsupported("requirePartitionFilter");
  }
}

// Tables do not expire in the emulator, so a Table resource cannot set an expiration; a null one,
// which clears it, is accepted.
void RejectExpiration(const json& table) {
  if (!Member(table, "expirationTime").is_null()) {
    throw Unsupported("table expiration");
  }
}

// Applies the description, friendly name and labels of a Table or Dataset resource `body` to
// `metadata`, as UpdateTableMetadata documents.
template <typename Metadata>
void UpdateDescriptiveFields(Metadata& metadata, const json& body, bool patch) {
  if (!patch) {
    metadata.labels.clear();
  }
  const auto set = [&](const char* key, auto apply) {
    if (const auto it = body.find(key); it != body.end()) {
      apply(*it);
    } else if (!patch) {
      apply(json());
    }
  };
  set("description",
      [&](const json& value) { metadata.description = StringValue(value, "description"); });
  set("friendlyName",
      [&](const json& value) { metadata.friendly_name = StringValue(value, "friendlyName"); });
  set("labels", [&](const json& value) { MergeLabels(value, metadata.labels); });
  ValidateLabels(metadata.labels);
}

// Throws for the fields of a Dataset resource the emulator does not keep; see
// DatasetMetadataFromJson. A null field, or an empty object or array such as the resourceTags bq
// sends, is accepted, since it sets nothing.
void CheckDatasetFields(const json& dataset) {
  if (!dataset.is_object()) {
    throw ApiError::Invalid("Invalid dataset resource");
  }
  static constexpr std::string_view kKnown[] = {"kind",
                                                "etag",
                                                "id",
                                                "selfLink",
                                                "datasetReference",
                                                "creationTime",
                                                "lastModifiedTime",
                                                "description",
                                                "friendlyName",
                                                "labels"};
  for (const auto& [key, value] : dataset.items()) {
    if (value.is_null() || ((value.is_object() || value.is_array()) && value.empty()) ||
        std::ranges::find(kKnown, key) != std::end(kKnown)) {
      continue;
    }
    if (key != "location") {
      throw Unsupported("dataset field " + key);
    }
    if (ToUpperAscii(StringValue(value, "location")) != "US") {
      throw Unsupported("dataset location " + value.get<std::string>());
    }
  }
}

// The top-level field of `schema` named `name` in any case, or null.
const FieldSchema* FindField(const std::vector<FieldSchema>& schema, const std::string& name) {
  const auto it = std::ranges::find_if(schema, [&](const FieldSchema& field) {
    return std::ranges::equal(field.name, name, [](char a, char b) {
      return std::tolower(static_cast<unsigned char>(a)) ==
             std::tolower(static_cast<unsigned char>(b));
    });
  });
  return it == schema.end() ? nullptr : &*it;
}

void ValidateTimePartitioning(const TimePartitioning& partitioning,
                              const std::vector<FieldSchema>& schema) {
  const FieldSchema* field = FindField(schema, partitioning.field);
  if (field == nullptr) {
    throw ApiError::Invalid("The field specified for partitioning cannot be found in the schema: " +
                            partitioning.field);
  }
  const bool time = field->type == FieldType::kTimestamp || field->type == FieldType::kDatetime;
  if ((!time && field->type != FieldType::kDate) || field->mode == FieldMode::kRepeated) {
    throw ApiError::Invalid(std::format(
        "The field specified for time partitioning can only be of type TIMESTAMP, DATE or "
        "DATETIME. The type found is: {}.",
        FieldTypeName(field->type)));
  }
  if (!time && partitioning.type == "HOUR") {
    throw ApiError::Invalid("A DATE field cannot be partitioned by HOUR: " + field->name);
  }
}

void ValidateRangePartitioning(const RangePartitioning& partitioning,
                               const std::vector<FieldSchema>& schema) {
  const FieldSchema* field = FindField(schema, partitioning.field);
  if (field == nullptr) {
    throw ApiError::Invalid("The field specified for partitioning cannot be found in the schema: " +
                            partitioning.field);
  }
  if (field->type != FieldType::kInteger || field->mode == FieldMode::kRepeated) {
    throw ApiError::Invalid(std::format(
        "The field specified for range partitioning can only be of type INTEGER. The type found "
        "is: {}.",
        FieldTypeName(field->type)));
  }
  if (partitioning.interval <= 0 || partitioning.end <= partitioning.start) {
    throw ApiError::Invalid(
        "Range partitioning needs a positive interval and an end after its start");
  }
  // The partition count, rounded up, without overflowing: end - start may exceed INT64_MAX.
  const auto span =
      static_cast<uint64_t>(partitioning.end) - static_cast<uint64_t>(partitioning.start);
  if ((span - 1) / static_cast<uint64_t>(partitioning.interval) + 1 >
      static_cast<uint64_t>(kMaxRangePartitions)) {
    throw ApiError::Invalid("Range partitioning cannot have more than 10000 partitions");
  }
}

void ValidateClustering(const std::vector<std::string>& clustering,
                        const std::vector<FieldSchema>& schema) {
  if (clustering.size() > kMaxClusteringFields) {
    throw ApiError::Invalid(
        std::format("Too many clustering fields: {}, only 4 allowed", clustering.size()));
  }
  for (const std::string& name : clustering) {
    const FieldSchema* field = FindField(schema, name);
    if (field == nullptr) {
      throw ApiError::Invalid("The field specified for clustering cannot be found in the schema: " +
                              name);
    }
    static constexpr FieldType kClusterable[] = {
        FieldType::kBigNumeric, FieldType::kBoolean,   FieldType::kDate,
        FieldType::kDatetime,   FieldType::kGeography, FieldType::kInteger,
        FieldType::kNumeric,    FieldType::kString,    FieldType::kTimestamp};
    if (std::ranges::find(kClusterable, field->type) == std::end(kClusterable) ||
        field->mode == FieldMode::kRepeated) {
      throw ApiError::Invalid(std::format("Field {} of type {} cannot be a clustering field",
                                          field->name, FieldTypeName(field->type)));
    }
  }
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
  if (time_partitioning.has_value()) {
    fields["timePartitioning"] = {{"type", time_partitioning->type},
                                  {"field", time_partitioning->field}};
  }
  if (range_partitioning.has_value()) {
    fields["rangePartitioning"] = {{"field", range_partitioning->field},
                                   {"range",
                                    {{"start", std::to_string(range_partitioning->start)},
                                     {"end", std::to_string(range_partitioning->end)},
                                     {"interval", std::to_string(range_partitioning->interval)}}}};
  }
  if (!clustering.empty()) {
    fields["clustering"] = {{"fields", clustering}};
  }
  return fields;
}

json DatasetMetadata::ToJson() const {
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
  RejectExpiration(table);
  ParsePartitioning(table, metadata);
  UpdateTableMetadata(metadata, table, /*patch=*/false);
  return metadata;
}

void UpdateTableMetadata(TableMetadata& metadata, const json& body, bool patch) {
  RejectExpiration(body);
  TableMetadata partitioning;
  ParsePartitioning(body, partitioning);
  if (partitioning.partitioned() &&
      (partitioning.time_partitioning != metadata.time_partitioning ||
       partitioning.range_partitioning != metadata.range_partitioning)) {
    throw ApiError::Invalid("Cannot change the partitioning of an existing table");
  }
  UpdateDescriptiveFields(metadata, body, patch);
  if (const auto it = body.find("clustering"); it != body.end()) {
    metadata.clustering = ParseClustering(*it);
  } else if (!patch) {
    metadata.clustering.clear();
  }
}

DatasetMetadata DatasetMetadataFromJson(const json& dataset) {
  DatasetMetadata metadata;
  UpdateDatasetMetadata(metadata, dataset, /*patch=*/false);
  return metadata;
}

void UpdateDatasetMetadata(DatasetMetadata& metadata, const json& body, bool patch) {
  CheckDatasetFields(body);
  UpdateDescriptiveFields(metadata, body, patch);
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

void ValidatePartitioning(const TableMetadata& metadata, const std::vector<FieldSchema>& schema) {
  if (metadata.time_partitioning.has_value()) {
    ValidateTimePartitioning(*metadata.time_partitioning, schema);
  }
  if (metadata.range_partitioning.has_value()) {
    ValidateRangePartitioning(*metadata.range_partitioning, schema);
  }
  ValidateClustering(metadata.clustering, schema);
}

}  // namespace bigquery_emulator_duckdb
