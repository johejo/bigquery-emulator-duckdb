#include "src/table_metadata.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <format>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "nlohmann/json.hpp"
#include "src/api_error.h"
#include "src/duckdb_sql.h"
#include "src/field_schema.h"
#include "src/location.h"

namespace bigquery_emulator_duckdb {

using nlohmann::json;

std::string_view TimePartitionUnitName(TimePartitionUnit unit) {
  switch (unit) {
    case TimePartitionUnit::kDay:
      return "DAY";
    case TimePartitionUnit::kHour:
      return "HOUR";
    case TimePartitionUnit::kMonth:
      return "MONTH";
    case TimePartitionUnit::kYear:
      return "YEAR";
  }
  return "";
}

std::optional<TimePartitionUnit> ParseTimePartitionUnit(std::string_view name) {
  for (const auto unit : {
           TimePartitionUnit::kDay,
           TimePartitionUnit::kHour,
           TimePartitionUnit::kMonth,
           TimePartitionUnit::kYear,
       }) {
    if (name == TimePartitionUnitName(unit)) {
      return unit;
    }
  }
  return std::nullopt;
}

namespace {

constexpr size_t kMaxLabels = 64;
constexpr size_t kMaxLabelLength = 63;
constexpr size_t kMaxClusteringFields = 4;
constexpr int64_t kMaxRangePartitions = 10000;
constexpr int64_t kMinDefaultTableExpirationMs = 3600000;

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
void MergeLabels(const json& labels, std::map<std::string, std::string>& into,
                 std::vector<std::string>& order) {
  if (labels.is_null()) {
    return;
  }
  if (!labels.is_object()) {
    throw ApiError::Invalid("Field labels must be an object");
  }
  for (const auto& [key, value] : labels.items()) {
    if (value.is_null()) {
      into.erase(key);
      std::erase(order, key);
    } else {
      if (!into.contains(key)) {
        order.push_back(key);
      }
      into[key] = StringValue(value, "labels");
    }
  }
}

// The characters of UTF-8 `text`: its bytes other than continuation bytes.
size_t CharacterCount(const std::string& text) {
  return std::ranges::count_if(
      text, [](char c) { return (static_cast<unsigned char>(c) & 0xC0U) != 0x80; });
}

bool LabelCharacter(char c) {
  return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-' ||
         static_cast<unsigned char>(c) >= 0x80;
}

// What is wrong with a label key or value `text`, or "" when nothing is.
std::string_view LabelTextProblem(const std::string& text) {
  if (CharacterCount(text) > kMaxLabelLength) {
    return "is longer than 63 characters";
  }
  if (!std::ranges::all_of(text, LabelCharacter)) {
    return "can contain only lowercase letters, numeric characters, underscores and dashes";
  }
  return "";
}

// An int64 member of `value`, which BigQuery encodes as a decimal string; a JSON number is
// accepted too.
int64_t Int64Member(const json& value, const char* key) {
  const auto it = value.find(key);
  if (it != value.end() && it->is_number_integer() &&
      (!it->is_number_unsigned() ||
       it->get<uint64_t>() <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))) {
    return it->get<int64_t>();
  }
  if (it != value.end() && it->is_string()) {
    const auto& text = it->get_ref<const std::string&>();
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
  const std::string name = StringValue(value.value("type", json()), "type");
  const auto unit = ParseTimePartitionUnit(name);
  TimePartitioning partitioning{
      .field = StringValue(value.value("field", json()), "field"),
  };
  if (!unit) {
    throw ApiError::Invalid("Invalid time partitioning type: " + name);
  }
  partitioning.type = *unit;
  if (partitioning.field.empty()) {
    throw ApiError::Unsupported("ingestion-time partitioning");
  }
  if (!value.value("expirationMs", json()).is_null()) {
    throw ApiError::Unsupported("partition expiration");
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
  return RangePartitioning{
      .field = StringValue(value.value("field", json()), "field"),
      .start = Int64Member(range, "start"),
      .end = Int64Member(range, "end"),
      .interval = Int64Member(range, "interval"),
  };
}

std::vector<std::string> ParseClustering(const json& value) {
  if (value.is_null()) {
    return {};
  }
  if (!value.is_object() || !value.value("fields", json::array()).is_array()) {
    throw ApiError::Invalid("Field clustering must be an object with an array of fields");
  }
  std::vector<std::string> fields;
  std::ranges::transform(value.value("fields", json::array()), std::back_inserter(fields),
                         [](const json& field) { return StringValue(field, "clustering.fields"); });
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
}

// Applies the description, friendly name, labels and default rounding mode of a Table or Dataset
// resource `body` to `metadata`, as UpdateTableMetadata documents.
template <typename Metadata>
void UpdateDescriptiveFields(Metadata& metadata, const json& body, bool patch, bool table) {
  // Every REST update sorts the existing labels before applying additions and deletions.
  metadata.label_order.clear();
  if (patch) {
    for (const auto& label : metadata.labels) {
      metadata.label_order.push_back(label.first);
    }
  }
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
  set("labels",
      [&](const json& value) { MergeLabels(value, metadata.labels, metadata.label_order); });
  if (const auto tags = body.find("resourceTags"); tags != body.end()) {
    if (patch && table && tags->is_null()) {
      metadata.resource_tags.clear();
      metadata.tag_order.clear();
    } else {
      if (tags->is_null()) {
        throw ApiError::Unsupported("null resourceTags on datasets or in full updates");
      }
      if (!tags->is_object()) {
        throw ApiError::Invalid("Field resourceTags must be an object");
      }
      if (patch && !table && tags->empty() && !metadata.resource_tags.empty()) {
        throw ApiError::Unsupported("empty resourceTags patches on tagged datasets");
      }
      if (!patch) {
        metadata.resource_tags.clear();
        metadata.tag_order.clear();
      } else if (metadata.tag_order.empty()) {
        for (const auto& [key, value] : metadata.resource_tags) {
          metadata.tag_order.push_back(key);
        }
      }
      for (const auto& [key, value] : tags->items()) {
        if (!value.is_string()) {
          throw ApiError::Invalid("Field resourceTags values must be strings");
        }
        if (patch && !metadata.resource_tags.contains(key)) {
          metadata.tag_order.push_back(key);
        }
        metadata.resource_tags[key] = value.get<std::string>();
      }
    }
  } else if (!patch) {
    metadata.resource_tags.clear();
    metadata.tag_order.clear();
  }
  ValidateResourceTags(metadata.resource_tags);
  set("defaultRoundingMode", [&](const json& value) {
    const std::string mode = StringValue(value, "defaultRoundingMode");
    std::optional<RoundingMode> parsed = ParseRoundingMode(mode);
    if (!parsed.has_value()) {
      throw ApiError::Invalid("Invalid default rounding mode: " + mode);
    }
    metadata.default_rounding_mode = *std::move(parsed);
  });
  ValidateLabels(metadata.labels);
}

// Throws for the fields of a Dataset resource the emulator does not keep; see
// DatasetMetadataFromJson. A null field, or an empty object or array, is accepted,
// since it sets nothing.
void CheckDatasetFields(const json& dataset) {
  if (!dataset.is_object()) {
    throw ApiError::Invalid("Invalid dataset resource");
  }
  static constexpr std::string_view kKnown[] = {
      "kind",
      "etag",
      "id",
      "selfLink",
      "datasetReference",
      "creationTime",
      "lastModifiedTime",
      "description",
      "friendlyName",
      "labels",
      "resourceTags",
      "tags",
      "defaultTableExpirationMs",
      "defaultRoundingMode",
      "location",
  };
  for (const auto& [key, value] : dataset.items()) {
    if (value.is_null() || ((value.is_object() || value.is_array()) && value.empty()) ||
        std::ranges::find(kKnown, key) != std::end(kKnown)) {
      continue;
    }
    throw ApiError::Unsupported("dataset field " + key);
  }
}

// The location the Dataset resource `dataset` names, or nothing when it names none.
std::optional<std::string> DatasetLocationField(const json& dataset) {
  const json value = Member(dataset, "location");
  if (value.is_null()) {
    return std::nullopt;
  }
  const std::string location = StringValue(value, "location");
  std::optional<std::string> canonical = CanonicalLocation(location);
  if (!canonical.has_value()) {
    throw ApiError::Invalid("Invalid location: " + location);
  }
  return canonical;
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
  if (!time && partitioning.type == TimePartitionUnit::kHour) {
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
  if (((span - 1) / static_cast<uint64_t>(partitioning.interval)) + 1 >
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
        FieldType::kBigNumeric, FieldType::kBoolean,   FieldType::kDate,    FieldType::kDatetime,
        FieldType::kGeography,  FieldType::kInteger,   FieldType::kNumeric, FieldType::kRange,
        FieldType::kString,     FieldType::kTimestamp,
    };
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
  if (creation_time != 0) {
    fields["creationTime"] = std::to_string(creation_time);
  }
  if (!description.empty()) {
    fields["description"] = description;
  }
  if (!friendly_name.empty()) {
    fields["friendlyName"] = friendly_name;
  }
  if (!labels.empty()) {
    fields["labels"] = labels;
  }
  if (!resource_tags.empty()) {
    fields["resourceTags"] = resource_tags;
  }
  if (time_partitioning.has_value()) {
    fields["timePartitioning"] = {
        {"type", TimePartitionUnitName(time_partitioning->type)},
        {"field", time_partitioning->field},
    };
  }
  if (range_partitioning.has_value()) {
    fields["rangePartitioning"] = {
        {"field", range_partitioning->field},
        {
            "range",
            {
                {"start", std::to_string(range_partitioning->start)},
                {"end", std::to_string(range_partitioning->end)},
                {"interval", std::to_string(range_partitioning->interval)},
            },
        },
    };
  }
  if (require_partition_filter) {
    fields["requirePartitionFilter"] = true;
    if (time_partitioning.has_value()) {
      fields["timePartitioning"]["requirePartitionFilter"] = true;
    }
  }
  if (!clustering.empty()) {
    fields["clustering"] = {{"fields", clustering}};
  }
  if (default_rounding_mode != RoundingMode::kUnspecified) {
    fields["defaultRoundingMode"] = RoundingModeName(default_rounding_mode);
  }
  if (expiration_time.has_value()) {
    fields["expirationTime"] = std::to_string(*expiration_time);
  }
  const auto base_table = [](const BaseTableDefinition& definition, const char* time) {
    return json{
        {
            "baseTableReference",
            {
                {"projectId", definition.base_table.project_id},
                {"datasetId", definition.base_table.dataset_id},
                {"tableId", definition.base_table.table_id},
            },
        },
        {time, definition.time},
    };
  };
  if (clone.has_value()) {
    fields["cloneDefinition"] = base_table(*clone, "cloneTime");
  }
  if (snapshot.has_value()) {
    fields["snapshotDefinition"] = base_table(*snapshot, "snapshotTime");
  }
  return fields;
}

json DatasetMetadata::ToJson() const {
  json fields = json::object();
  if (creation_time != 0) {
    fields["creationTime"] = std::to_string(creation_time);
  }
  if (!description.empty()) {
    fields["description"] = description;
  }
  if (!friendly_name.empty()) {
    fields["friendlyName"] = friendly_name;
  }
  if (!labels.empty()) {
    fields["labels"] = labels;
  }
  if (!resource_tags.empty()) {
    fields["resourceTags"] = resource_tags;
    fields["tags"] = json::array();
    const auto append = [&](const std::string& key) {
      if (const auto tag = resource_tags.find(key); tag != resource_tags.end()) {
        fields["tags"].push_back({{"tagKey", tag->first}, {"tagValue", tag->second}});
      }
    };
    if (tag_order.empty()) {
      for (const auto& [key, value] : resource_tags) {
        append(key);
      }
    } else {
      for (const std::string& key : tag_order) {
        append(key);
      }
    }
  }
  if (default_table_expiration_ms.has_value()) {
    fields["defaultTableExpirationMs"] = std::to_string(*default_table_expiration_ms);
  }
  if (default_rounding_mode != RoundingMode::kUnspecified) {
    fields["defaultRoundingMode"] = RoundingModeName(default_rounding_mode);
  }
  if (!location.empty()) {
    fields["location"] = location;
  }
  return fields;
}

TableMetadata TableMetadataFromJson(const json& table) {
  TableMetadata metadata;
  ParsePartitioning(table, metadata);
  UpdateTableMetadata(metadata, table, /*patch=*/false);
  return metadata;
}

void RejectEncryptionConfiguration(const json& body, const char* key, const std::string& what) {
  if (const json value = Member(body, key); !value.is_null() && value != json::object()) {
    throw ApiError::Unsupported(what);
  }
}

void UpdateTableMetadata(TableMetadata& metadata, const json& body, bool patch) {
  RejectEncryptionConfiguration(body, "encryptionConfiguration",
                                "table field encryptionConfiguration");
  TableMetadata partitioning;
  ParsePartitioning(body, partitioning);
  if (partitioning.partitioned() &&
      (partitioning.time_partitioning != metadata.time_partitioning ||
       partitioning.range_partitioning != metadata.range_partitioning)) {
    throw ApiError::Invalid("Cannot change the partitioning of an existing table");
  }
  const json required =
      body.contains("requirePartitionFilter")
          ? Member(body, "requirePartitionFilter")
          : Member(Member(body, "timePartitioning").is_object() ? Member(body, "timePartitioning")
                                                                : json::object(),
                   "requirePartitionFilter");
  if (!required.is_null() && !required.is_boolean()) {
    throw ApiError::Invalid("Field requirePartitionFilter must be a boolean");
  }
  if (!required.is_null() || !patch || body.contains("requirePartitionFilter")) {
    metadata.require_partition_filter = required == true;
  }
  UpdateDescriptiveFields(metadata, body, patch, /*table=*/true);
  if (body.contains("expirationTime")) {
    metadata.expiration_time = Member(body, "expirationTime").is_null()
                                   ? std::nullopt
                                   : std::optional(Int64Member(body, "expirationTime"));
  } else if (!patch) {
    metadata.expiration_time.reset();
  }
  if (const auto it = body.find("clustering"); it != body.end()) {
    metadata.clustering = ParseClustering(*it);
  } else if (!patch) {
    metadata.clustering.clear();
  }
}

DatasetMetadata DatasetMetadataFromJson(const json& dataset) {
  DatasetMetadata metadata;
  CheckDatasetFields(dataset);
  metadata.location = DatasetLocationField(dataset).value_or(kDefaultLocation);
  UpdateDatasetMetadata(metadata, dataset, /*patch=*/false);
  return metadata;
}

void UpdateDatasetMetadata(DatasetMetadata& metadata, const json& body, bool patch) {
  CheckDatasetFields(body);
  // Whether BigQuery rejects another location or ignores it is not known.
  if (const std::optional<std::string> location = DatasetLocationField(body);
      location.has_value() && *location != metadata.Location()) {
    throw ApiError::Unsupported("changing the location of a dataset");
  }
  UpdateDescriptiveFields(metadata, body, patch, /*table=*/false);
  // Dataset REST ordering is unverified; retain its existing key ordering.
  metadata.label_order.clear();
  if (body.contains("defaultTableExpirationMs") || !patch) {
    const bool unset = Member(body, "defaultTableExpirationMs").is_null() ||
                       Int64Member(body, "defaultTableExpirationMs") == 0;
    metadata.default_table_expiration_ms =
        unset ? std::nullopt : std::optional(Int64Member(body, "defaultTableExpirationMs"));
  }
  ValidateDefaultTableExpiration(metadata.default_table_expiration_ms);
}

void ValidateDefaultTableExpiration(const std::optional<int64_t>& milliseconds) {
  if (milliseconds.has_value() && *milliseconds < kMinDefaultTableExpirationMs) {
    throw ApiError::Invalid(
        std::format("The default table expiration must be at least 3600000 milliseconds (one "
                    "hour), but was {}",
                    *milliseconds));
  }
}

std::optional<int64_t> ExpirationWithDefault(const std::optional<int64_t>& expiration_time,
                                             int64_t creation_time,
                                             const DatasetMetadata& dataset) {
  if (expiration_time.has_value() || !dataset.default_table_expiration_ms.has_value()) {
    return expiration_time;
  }
  if (*dataset.default_table_expiration_ms > std::numeric_limits<int64_t>::max() - creation_time) {
    throw ApiError::Invalid("The default table expiration of the dataset is out of range");
  }
  return creation_time + *dataset.default_table_expiration_ms;
}

void ValidateResourceTags(const std::map<std::string, std::string>& tags) {
  if (tags.size() > 50) {
    throw ApiError::Invalid("A resource can have at most 50 tags");
  }
  for (const auto& [key, value] : tags) {
    const auto slash = key.find('/');
    if (slash == std::string::npos || slash == 0 || slash + 1 == key.size() ||
        key.find('/', slash + 1) != std::string::npos || value.empty()) {
      throw ApiError::Invalid("Tags require a namespaced key and a nonempty short value name");
    }
  }
}

void ValidateLabels(const std::map<std::string, std::string>& labels) {
  if (labels.size() > kMaxLabels) {
    throw ApiError::Invalid("A resource can have at most 64 labels");
  }
  for (const auto& [key, value] : labels) {
    const std::string what = "Label key \"" + key + "\"";
    if (key.empty() ||
        ((key.at(0) < 'a' || key.at(0) > 'z') && static_cast<unsigned char>(key.at(0)) < 0x80)) {
      throw ApiError::Invalid(what + " must start with a lowercase letter");
    }
    if (const std::string_view problem = LabelTextProblem(key); !problem.empty()) {
      throw ApiError::Invalid(std::format("{} {}", what, problem));
    }
    if (const std::string_view problem = LabelTextProblem(value); !problem.empty()) {
      throw ApiError::Invalid(std::format("Label value \"{}\" {}", value, problem));
    }
  }
}

void ValidatePartitioning(const TableMetadata& metadata, const std::vector<FieldSchema>& schema) {
  if (metadata.require_partition_filter && !metadata.partitioned()) {
    throw ApiError::Unsupported("requirePartitionFilter on an unpartitioned table");
  }
  if (metadata.time_partitioning.has_value()) {
    ValidateTimePartitioning(*metadata.time_partitioning, schema);
  }
  if (metadata.range_partitioning.has_value()) {
    ValidateRangePartitioning(*metadata.range_partitioning, schema);
  }
  ValidateClustering(metadata.clustering, schema);
}

std::string DatasetMetadataTable(const std::string& project) {
  return QuoteIdentifier(project) + ".main.emulator_datasets";
}

}  // namespace bigquery_emulator_duckdb
