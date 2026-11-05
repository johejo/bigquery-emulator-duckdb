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
#include "src/json_int64.h"
#include "src/location.h"
#include "src/text_encoding.h"

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

// Applies the labels of `labels`, a JSON object, to `into`; a null value removes a label, and
// null labels remove them all, as in a patch.
void MergeLabels(const json& labels, std::map<std::string, std::string>& into,
                 std::vector<std::string>& order) {
  if (labels.is_null()) {
    into.clear();
    order.clear();
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
  if (it != value.end()) {
    if (const auto parsed = ParseJsonInt64(*it)) {
      return *parsed;
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

// TableConstraints.primaryKey.columns of `value`, a TableConstraints object or null; its
// foreignKeys are dropped.
std::vector<std::string> ParsePrimaryKey(const json& value) {
  const json primary_key = value.is_object() ? Member(value, "primaryKey") : json();
  const json columns = primary_key.is_object() ? Member(primary_key, "columns") : json();
  if ((!value.is_null() && !value.is_object()) ||
      (!primary_key.is_null() && !primary_key.is_object()) ||
      (!columns.is_null() && !columns.is_array())) {
    throw ApiError::Invalid("Field tableConstraints.primaryKey.columns must be an array");
  }
  std::vector<std::string> names;
  std::ranges::transform(columns.is_array() ? columns : json::array(), std::back_inserter(names),
                         [](const json& column) {
                           return StringValue(column, "tableConstraints.primaryKey.columns");
                         });
  return names;
}

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
  // As BigQuery answers, datasets.update keeps a friendly name it leaves out, but clears one it
  // sets to null.
  if (table || body.contains("friendlyName")) {
    set("friendlyName",
        [&](const json& value) { metadata.friendly_name = StringValue(value, "friendlyName"); });
  }
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
  // BigQuery's recorded answers: tables keep their default for null or an omitted field,
  // even on full updates. Datasets clear it to an explicit empty default for null or a full
  // update that omits it.
  if (const auto collation = body.find("defaultCollation"); collation != body.end()) {
    if (!collation->is_null() || !table) {
      metadata.default_collation = StringValue(*collation, "defaultCollation");
      ValidateCollation(*metadata.default_collation);
    }
  } else if (!table && !patch && metadata.default_collation.has_value()) {
    metadata.default_collation = "";
  }
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
    // BigQuery's message, where
    // third_party/bigquery_docs/reference/standard-sql/collation-concepts.md documents "CLUSTER BY
    // STRING column word with collation und:ci isn't supported".
    if (field->collation.has_value() && !field->collation->empty()) {
      throw ApiError::Invalid(
          std::format("Fields specified for clustering cannot have collation. Invalid field: {}.",
                      field->name));
    }
  }
}

}  // namespace

json BigtableOptionsJson(const BigtableOptions& options) {
  json result = json::object();
  if (options.ignore_unspecified_column_families) {
    result["ignoreUnspecifiedColumnFamilies"] = *options.ignore_unspecified_column_families;
  }
  if (options.read_rowkey_as_string) {
    result["readRowkeyAsString"] = *options.read_rowkey_as_string;
  }
  for (const auto& family : options.column_families) {
    json value = {{"familyId", family.family_id}};
    if (!family.type.empty()) {
      value["type"] = family.type;
    }
    if (family.encoding) {
      value["encoding"] = *family.encoding;
    }
    if (family.only_read_latest) {
      value["onlyReadLatest"] = *family.only_read_latest;
    }
    result["columnFamilies"].push_back(std::move(value));
  }
  return result;
}

json TableMetadata::ToJson() const {
  json fields = json::object();
  if (external_data) {
    fields["externalDataConfiguration"] = {
        {"sourceFormat", external_data->format},
        {"sourceUris", external_data->uris},
    };
    if (external_data->bigtable_options) {
      fields["externalDataConfiguration"]["bigtableOptions"] =
          BigtableOptionsJson(*external_data->bigtable_options);
    }
    if (external_data->skip_leading_rows) {
      fields["externalDataConfiguration"]["csvOptions"]["skipLeadingRows"] =
          std::to_string(*external_data->skip_leading_rows);
    }
    if (external_data->field_delimiter) {
      fields["externalDataConfiguration"]["csvOptions"]["fieldDelimiter"] =
          *external_data->field_delimiter;
    }
    json& csv = fields["externalDataConfiguration"]["csvOptions"];
    if (external_data->quote) {
      csv["quote"] = *external_data->quote;
    }
    if (external_data->allow_quoted_newlines) {
      csv["allowQuotedNewlines"] = *external_data->allow_quoted_newlines;
    }
    if (external_data->allow_jagged_rows) {
      csv["allowJaggedRows"] = *external_data->allow_jagged_rows;
    }
    if (external_data->null_marker) {
      csv["nullMarker"] = *external_data->null_marker;
    }
    if (external_data->null_markers) {
      csv["nullMarkers"] = *external_data->null_markers;
    }
    if (external_data->preserve_ascii_control_characters) {
      csv["preserveAsciiControlCharacters"] = *external_data->preserve_ascii_control_characters;
    }
    if (external_data->encoding) {
      fields["externalDataConfiguration"]
            [external_data->format == "CSV" ? "csvOptions" : "jsonOptions"]["encoding"] =
                *external_data->encoding;
    }
    if (csv.is_null()) {
      fields["externalDataConfiguration"].erase("csvOptions");
    }
    if (external_data->ignore_unknown_values) {
      fields["externalDataConfiguration"]["ignoreUnknownValues"] =
          *external_data->ignore_unknown_values;
    }
    if (external_data->compression) {
      fields["externalDataConfiguration"]["compression"] = *external_data->compression;
    }
    if (external_data->max_bad_records) {
      fields["externalDataConfiguration"]["maxBadRecords"] = *external_data->max_bad_records;
    }
    for (const auto& [name, value] : {
             std::pair{"timeZone", &external_data->time_zone},
             {"dateFormat", &external_data->date_format},
             {"datetimeFormat", &external_data->datetime_format},
             {"timeFormat", &external_data->time_format},
             {"timestampFormat", &external_data->timestamp_format},
         }) {
      if (*value) {
        fields["externalDataConfiguration"][name] = **value;
      }
    }
    if (external_data->autodetect.has_value()) {
      fields["externalDataConfiguration"]["autodetect"] = *external_data->autodetect;
    }
    if (external_data->enable_list_inference.has_value()) {
      fields["externalDataConfiguration"]["parquetOptions"]["enableListInference"] =
          *external_data->enable_list_inference;
    }
    if (const auto& hive = external_data->hive_partitioning) {
      json options = json::object();
      if (!hive->fields.empty()) {
        options["fields"] = hive->fields;
      }
      if (!hive->mode.empty()) {
        options["mode"] = hive->mode;
      }
      if (hive->require_partition_filter.value_or(false)) {
        options["requirePartitionFilter"] = true;
      }
      if (!hive->source_uri_prefix.empty()) {
        options["sourceUriPrefix"] = hive->source_uri_prefix;
      }
      fields["externalDataConfiguration"]["hivePartitioningOptions"] = std::move(options);
    }
  }
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
  if (!primary_key.empty()) {
    fields["tableConstraints"] = {{"primaryKey", {{"columns", primary_key}}}};
  }
  if (default_rounding_mode != RoundingMode::kUnspecified) {
    fields["defaultRoundingMode"] = RoundingModeName(default_rounding_mode);
  }
  if (default_collation.has_value()) {
    fields["defaultCollation"] = *default_collation;
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
  if (max_time_travel_hours.has_value()) {
    fields["maxTimeTravelHours"] = std::to_string(*max_time_travel_hours);
  }
  if (!storage_billing_model.empty()) {
    fields["storageBillingModel"] = storage_billing_model;
  }
  if (default_rounding_mode != RoundingMode::kUnspecified) {
    fields["defaultRoundingMode"] = RoundingModeName(default_rounding_mode);
  }
  if (default_collation.has_value()) {
    fields["defaultCollation"] = *default_collation;
  }
  if (!location.empty()) {
    fields["location"] = location;
  }
  return fields;
}

namespace {

// hivePartitioningOptions. BigQuery stores options without a mode, which do not partition the
// table, and recomputes the output-only fields when it creates a table.
HivePartitioning HivePartitioningFromJson(const json& options) {
  if (!options.is_object()) {
    throw ApiError::Invalid("externalDataConfiguration.hivePartitioningOptions must be an object");
  }
  HivePartitioning hive;
  for (const auto& [name, value] : options.items()) {
    if (name == "mode" && value.is_string()) {
      hive.mode = value.get<std::string>();
    } else if (name == "sourceUriPrefix" && value.is_string()) {
      hive.source_uri_prefix = value.get<std::string>();
    } else if (name == "requirePartitionFilter" && value.is_boolean()) {
      hive.require_partition_filter = value.get<bool>();
    } else if (name == "fields" && value.is_array() &&
               std::ranges::all_of(value, [](const json& field) { return field.is_string(); })) {
      hive.fields = value.get<std::vector<std::string>>();
    } else {
      throw ApiError::Invalid("Invalid externalDataConfiguration.hivePartitioningOptions." + name);
    }
  }
  if (!hive.mode.empty() && hive.mode != "AUTO" && hive.mode != "STRINGS" &&
      hive.mode != "CUSTOM") {
    throw ApiError::Invalid("Invalid value for hivePartitioning.mode: " + hive.mode +
                            " is not a valid value");
  }
  if (!hive.mode.empty() && hive.source_uri_prefix.empty()) {
    throw ApiError::Invalid(
        "Hive partitioned queries require that the HivePartitioningOptions be set, with both a "
        "sourceUriPrefix and mode provided. No sourceUriPrefix was provided.");
  }
  return hive;
}

// The encoding `value` of csvOptions or jsonOptions, `options`. BigQuery's answers in
// external_table_read_options.api.answers.json: tables.insert takes only the names tables.get
// gives.
std::string RestEncoding(const json& value, std::string_view options) {
  const auto& name = value.get_ref<const std::string&>();
  if (!ParseTextEncoding(name)) {
    throw ApiError::Invalid(
        std::format("Invalid value for {}.encoding: {} is not a valid value", options, name));
  }
  return name;
}

}  // namespace

TableMetadata TableMetadataFromJson(const json& table) {
  TableMetadata metadata;
  if (table.contains("externalDataConfiguration") &&
      !table["externalDataConfiguration"].is_null()) {
    const auto& config = table["externalDataConfiguration"];
    if (!config.is_object()) {
      throw ApiError::Invalid("externalDataConfiguration must be an object");
    }
    for (const auto& [name, value] : config.items()) {
      if (name != "sourceFormat" && name != "sourceUris" && name != "schema" &&
          name != "autodetect" && name != "parquetOptions" && name != "csvOptions" &&
          name != "ignoreUnknownValues" && name != "hivePartitioningOptions" &&
          name != "compression" && name != "maxBadRecords" && name != "jsonOptions" &&
          name != "timeZone" && name != "dateFormat" && name != "datetimeFormat" &&
          name != "timeFormat" && name != "timestampFormat" && name != "bigtableOptions") {
        throw ApiError::Unsupported("externalDataConfiguration." + name);
      }
    }
    const std::string format = config.value("sourceFormat", "");
    if (format != "PARQUET" && format != "CSV" && format != "NEWLINE_DELIMITED_JSON" &&
        format != "BIGTABLE") {
      throw ApiError::Unsupported("external tables other than PARQUET, CSV, JSON or BIGTABLE");
    }
    const auto uris = config.value("sourceUris", json::array());
    if (!uris.is_array() || uris.empty()) {
      throw ApiError::Invalid("externalDataConfiguration.sourceUris must not be empty");
    }
    metadata.external_data.emplace();
    metadata.external_data->format = format;
    if (format == "BIGTABLE") {
      for (const auto& [name, value] : config.items()) {
        if (name != "sourceFormat" && name != "sourceUris" && name != "bigtableOptions") {
          throw ApiError::Unsupported("Bigtable external table option " + name);
        }
      }
    }
    if (config.contains("bigtableOptions")) {
      if (format != "BIGTABLE") {
        throw ApiError::Unsupported("bigtableOptions on a non-Bigtable external table");
      }
      const auto& options = config["bigtableOptions"];
      if (!options.is_object()) {
        throw ApiError::Invalid("bigtableOptions must be an object");
      }
      auto& parsed = metadata.external_data->bigtable_options.emplace();
      for (const auto& [name, value] : options.items()) {
        if (name == "ignoreUnspecifiedColumnFamilies" || name == "readRowkeyAsString") {
          if (!value.is_boolean()) {
            throw ApiError::Invalid("bigtableOptions." + name + " must be a boolean");
          }
          (name == "readRowkeyAsString" ? parsed.read_rowkey_as_string
                                        : parsed.ignore_unspecified_column_families) =
              value.get<bool>();
        } else if (name == "columnFamilies") {
          if (!value.is_array()) {
            throw ApiError::Invalid("bigtableOptions.columnFamilies must be an array");
          }
          for (const auto& family : value) {
            if (!family.is_object()) {
              throw ApiError::Invalid("Bigtable column family must be an object");
            }
            BigtableFamily entry;
            for (const auto& [key, setting] : family.items()) {
              if (key == "familyId" || key == "type" || key == "encoding") {
                if (!setting.is_string()) {
                  throw ApiError::Invalid("Bigtable " + key + " must be a string");
                }
                if (key == "familyId") {
                  entry.family_id = setting.get<std::string>();
                } else if (key == "type") {
                  entry.type = setting.get<std::string>();
                  if (entry.type.empty()) {
                    throw ApiError::Unsupported("Empty Bigtable cell type");
                  }
                } else {
                  entry.encoding = setting.get<std::string>();
                }
              } else if (key == "onlyReadLatest") {
                if (!setting.is_boolean()) {
                  throw ApiError::Invalid("Bigtable onlyReadLatest must be a boolean");
                }
                entry.only_read_latest = setting.get<bool>();
              } else {
                throw ApiError::Unsupported("Bigtable column family option " + key);
              }
            }
            if (entry.family_id.empty()) {
              throw ApiError::Invalid("Bigtable familyId must not be empty");
            }
            if (!entry.type.empty() && entry.type != "BYTES" && entry.type != "STRING") {
              throw ApiError::Unsupported("Bigtable cell type " + entry.type);
            }
            if (entry.encoding && *entry.encoding != "BINARY" && *entry.encoding != "TEXT") {
              throw ApiError::Unsupported("Bigtable cell encoding " + *entry.encoding);
            }
            parsed.column_families.push_back(std::move(entry));
          }
        } else {
          throw ApiError::Unsupported("bigtableOptions." + name);
        }
      }
    }
    if (config.contains("csvOptions")) {
      if (format != "CSV") {
        throw ApiError::Unsupported("csvOptions on a non-CSV external table");
      }
      const auto& options = config["csvOptions"];
      if (!options.is_object()) {
        throw ApiError::Invalid("externalDataConfiguration.csvOptions must be an object");
      }
      for (const auto& [name, value] : options.items()) {
        if (name == "fieldDelimiter" && value.is_string()) {
          metadata.external_data->field_delimiter = value.get<std::string>();
        } else if (name == "skipLeadingRows" && value.is_number_integer()) {
          metadata.external_data->skip_leading_rows = value.get<int64_t>();
        } else if (name == "skipLeadingRows" && value.is_string()) {
          const auto& text = value.get_ref<const std::string&>();
          int64_t rows = 0;
          const auto result = std::from_chars(text.data(), text.data() + text.size(), rows);
          if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
            throw ApiError::Invalid("Invalid csvOptions.skipLeadingRows");
          }
          metadata.external_data->skip_leading_rows = rows;
        } else if (name == "quote" && value.is_string()) {
          metadata.external_data->quote = value.get<std::string>();
        } else if (name == "nullMarker" && value.is_string()) {
          metadata.external_data->null_marker = value.get<std::string>();
        } else if (name == "nullMarkers" && value.is_array() &&
                   std::ranges::all_of(value,
                                       [](const json& marker) { return marker.is_string(); })) {
          metadata.external_data->null_markers = value.get<std::vector<std::string>>();
        } else if (name == "allowQuotedNewlines" && value.is_boolean()) {
          metadata.external_data->allow_quoted_newlines = value.get<bool>();
        } else if (name == "allowJaggedRows" && value.is_boolean()) {
          metadata.external_data->allow_jagged_rows = value.get<bool>();
        } else if (name == "preserveAsciiControlCharacters" && value.is_boolean()) {
          metadata.external_data->preserve_ascii_control_characters = value.get<bool>();
        } else if (name == "encoding" && value.is_string()) {
          metadata.external_data->encoding = RestEncoding(value, "csvOptions");
        } else {
          throw ApiError::Unsupported("externalDataConfiguration.csvOptions." + name);
        }
      }
    }
    if (config.contains("jsonOptions")) {
      if (format != "NEWLINE_DELIMITED_JSON") {
        throw ApiError::Unsupported("jsonOptions on a non-JSON external table");
      }
      const auto& options = config["jsonOptions"];
      if (!options.is_object()) {
        throw ApiError::Invalid("externalDataConfiguration.jsonOptions must be an object");
      }
      for (const auto& [name, value] : options.items()) {
        if (name != "encoding") {
          throw ApiError::Unsupported("externalDataConfiguration.jsonOptions." + name);
        }
        if (!value.is_string()) {
          throw ApiError::Invalid(
              "externalDataConfiguration.jsonOptions.encoding must be a string");
        }
        metadata.external_data->encoding = RestEncoding(value, "jsonOptions");
      }
    }
    if (config.contains("compression")) {
      if (!config["compression"].is_string()) {
        throw ApiError::Invalid("externalDataConfiguration.compression must be a string");
      }
      // BigQuery stores no compression for an empty one.
      const auto& compression = config["compression"].get_ref<const std::string&>();
      if (!compression.empty() && compression != "GZIP" && compression != "NONE") {
        throw ApiError::Invalid("Invalid value for compression: " + compression +
                                " is not a valid value");
      }
      if (!compression.empty()) {
        metadata.external_data->compression = compression;
      }
    }
    if (config.contains("maxBadRecords")) {
      const auto& value = config["maxBadRecords"];
      int64_t records = 0;
      if (value.is_number_integer()) {
        records = value.get<int64_t>();
      } else if (value.is_string()) {
        const auto& text = value.get_ref<const std::string&>();
        const auto result = std::from_chars(text.data(), text.data() + text.size(), records);
        if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
          throw ApiError::Invalid("Invalid externalDataConfiguration.maxBadRecords");
        }
      } else {
        throw ApiError::Invalid("Invalid externalDataConfiguration.maxBadRecords");
      }
      if (records < 0) {
        throw ApiError::Invalid("must be a positive number.");
      }
      metadata.external_data->max_bad_records = records;
    }
    if (config.contains("parquetOptions")) {
      if (format != "PARQUET") {
        throw ApiError::Unsupported("parquetOptions on a non-Parquet external table");
      }
      const auto& options = config["parquetOptions"];
      if (!options.is_object()) {
        throw ApiError::Invalid("externalDataConfiguration.parquetOptions must be an object");
      }
      for (const auto& [name, value] : options.items()) {
        if (name != "enableListInference") {
          throw ApiError::Unsupported("externalDataConfiguration.parquetOptions." + name);
        }
        if (!value.is_boolean()) {
          throw ApiError::Invalid(
              "externalDataConfiguration.parquetOptions.enableListInference must be a boolean");
        }
        metadata.external_data->enable_list_inference = value.get<bool>();
      }
    }
    for (const auto& [name, value] : {
             std::pair{"timeZone", &metadata.external_data->time_zone},
             {"dateFormat", &metadata.external_data->date_format},
             {"datetimeFormat", &metadata.external_data->datetime_format},
             {"timeFormat", &metadata.external_data->time_format},
             {"timestampFormat", &metadata.external_data->timestamp_format},
         }) {
      if (config.contains(name)) {
        if (!config[name].is_string()) {
          throw ApiError::Invalid(std::string("externalDataConfiguration.") + name +
                                  " must be a string");
        }
        *value = config[name].get<std::string>();
      }
    }
    // As BigQuery's answers in external_table_types_followups.api.answers.json show, a Parquet
    // table rejects them by their DDL names.
    if (format == "PARQUET") {
      for (const auto& [name, value] : {
               std::pair{"time_zone", &metadata.external_data->time_zone},
               {"date_format", &metadata.external_data->date_format},
               {"datetime_format", &metadata.external_data->datetime_format},
               {"time_format", &metadata.external_data->time_format},
               {"timestamp_format", &metadata.external_data->timestamp_format},
           }) {
        if (*value) {
          throw ApiError::Invalid(
              std::format("Unsupported option '{}' for format STORAGE_FORMAT_PARQUET", name));
        }
      }
    }
    if (config.contains("hivePartitioningOptions")) {
      metadata.external_data->hive_partitioning =
          HivePartitioningFromJson(config["hivePartitioningOptions"]);
    }
    if (config.contains("ignoreUnknownValues")) {
      if (!config["ignoreUnknownValues"].is_boolean()) {
        throw ApiError::Invalid("externalDataConfiguration.ignoreUnknownValues must be a boolean");
      }
      metadata.external_data->ignore_unknown_values = config["ignoreUnknownValues"].get<bool>();
    }
    if (config.contains("autodetect")) {
      if (!config["autodetect"].is_boolean()) {
        throw ApiError::Invalid("externalDataConfiguration.autodetect must be a boolean");
      }
      // Without a schema, a Parquet table takes the one its sources have either way. A CSV
      // table keeps a given schema, and autodetection still finds the header its reads skip. A
      // JSON table keeps one too, but its reads skip fields the schema lacks.
      const auto given = [](const json& resource) {
        return resource.contains("schema") && !resource["schema"].is_null();
      };
      const bool given_schema = config["autodetect"] == true && (given(table) || given(config));
      if (given_schema && format != "CSV" && format != "NEWLINE_DELIMITED_JSON") {
        throw ApiError::Unsupported("external table schema autodetection with a schema");
      }
      metadata.external_data->autodetect = config["autodetect"].get<bool>();
      metadata.external_data->given_schema = given_schema && format == "NEWLINE_DELIMITED_JSON";
    }
    for (const auto& uri : uris) {
      if (!uri.is_string()) {
        throw ApiError::Invalid("external table source URI must be a string");
      }
      if (format != "BIGTABLE" && !uri.get_ref<const std::string&>().starts_with("gs://")) {
        throw ApiError::Unsupported("external table sources other than gs://");
      }
      metadata.external_data->uris.push_back(uri.get<std::string>());
    }
  }
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
  // As BigQuery answers, tables.patch keeps the primary key when tableConstraints is an object
  // without one.
  if (const auto it = body.find("tableConstraints");
      it != body.end() && (!patch || !it->is_object() || it->contains("primaryKey"))) {
    metadata.primary_key = ParsePrimaryKey(*it);
  } else if (!patch) {
    metadata.primary_key.clear();
  }
}

DatasetMetadata DatasetMetadataFromJson(const json& dataset) {
  DatasetMetadata metadata;
  metadata.location = DatasetLocationField(dataset).value_or(kDefaultLocation);
  UpdateDatasetMetadata(metadata, dataset, /*patch=*/false);
  return metadata;
}

void UpdateDatasetMetadata(DatasetMetadata& metadata, const json& body, bool patch) {
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
  if (body.contains("maxTimeTravelHours") && body["maxTimeTravelHours"].is_null()) {
    throw ApiError::Unsupported("null maxTimeTravelHours");
  }
  if (body.contains("maxTimeTravelHours") || !patch) {
    metadata.max_time_travel_hours = Member(body, "maxTimeTravelHours").is_null()
                                         ? std::nullopt
                                         : std::optional(Int64Member(body, "maxTimeTravelHours"));
  }
  ValidateMaxTimeTravelHours(metadata.max_time_travel_hours);
  // Omission on update must not silently change a dataset's billing model.
  if (body.contains("storageBillingModel")) {
    const json& model = body["storageBillingModel"];
    if (model.is_null() || model == "STORAGE_BILLING_MODEL_UNSPECIFIED") {
      return;
    }
    if (!model.is_string() || (model != "LOGICAL" && model != "PHYSICAL")) {
      throw ApiError::Unsupported("storageBillingModel other than LOGICAL or PHYSICAL");
    }
    metadata.storage_billing_model = model.get<std::string>();
  }
}

void ValidateMaxTimeTravelHours(const std::optional<int64_t>& hours) {
  if (hours && (*hours < 48 || *hours > 168 || *hours % 24 != 0)) {
    throw ApiError::Invalid("max_time_travel_hours must be a multiple of 24 between 48 and 168");
  }
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

std::vector<std::string> PrimaryKeyColumns(const std::vector<std::string>& primary_key,
                                           const std::vector<FieldSchema>& schema) {
  std::vector<std::string> columns;
  for (const std::string& name : primary_key) {
    const FieldSchema* field = FindField(schema, name);
    if (field == nullptr) {
      throw ApiError::Invalid("Primary key column " + name + " was not found.");
    }
    // BigQuery's answers; what it says of BOOL, DATE, TIME and DATETIME columns is not known.
    static constexpr FieldType kRejected[] = {
        FieldType::kFloat, FieldType::kGeography, FieldType::kInterval,
        FieldType::kJson,  FieldType::kRange,     FieldType::kRecord,
    };
    if (field->mode == FieldMode::kRepeated ||
        std::ranges::find(kRejected, field->type) != std::end(kRejected)) {
      throw ApiError::Invalid(std::format(
          "Primary key column cannot be of type {}. Invalid column: {}.",
          field->mode == FieldMode::kRepeated ? "ARRAY" : FieldTypeName(field->type), field->name));
    }
    columns.push_back(field->name);
  }
  return columns;
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
