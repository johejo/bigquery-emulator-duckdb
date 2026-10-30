#include "src/table_comments.h"

#include <algorithm>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"
#include "src/api_error.h"
#include "src/duckdb_sql.h"
#include "src/field_schema.h"
#include "src/gcs_uri.h"
#include "src/references.h"
#include "src/table_metadata.h"

namespace bigquery_emulator_duckdb {
namespace {

using nlohmann::json;

// The JSON object a comment holds, or null for a comment the emulator did not write.
json ParseComment(const std::optional<std::string>& comment) {
  if (!comment.has_value()) {
    return nullptr;
  }
  json parsed = json::parse(*comment, nullptr, /*allow_exceptions=*/false);
  return parsed.is_object() ? parsed : json(nullptr);
}

// Whether `field` or one of its fields has a type or setting that external Parquet reads do not
// detect.
bool UnsupportedExternalField(const FieldSchema& field) {
  return field.max_length || field.precision || field.scale || field.collation ||
         !field.default_value_expression.empty() ||
         (field.type != FieldType::kInteger && field.type != FieldType::kString &&
          field.type != FieldType::kBoolean && field.type != FieldType::kFloat &&
          field.type != FieldType::kBytes && field.type != FieldType::kNumeric &&
          field.type != FieldType::kDate && field.type != FieldType::kTime &&
          field.type != FieldType::kTimestamp && field.type != FieldType::kRecord) ||
         std::ranges::any_of(field.fields, UnsupportedExternalField);
}

// The cloneDefinition or snapshotDefinition `definition` that the emulator recorded in a table
// comment, which TableMetadataFromJson ignores since clients cannot set them; `time` names its
// time.
std::optional<BaseTableDefinition> ParseBaseTableDefinition(const json& definition,
                                                            const char* time) {
  if (!definition.is_object()) {
    return std::nullopt;
  }
  const json base = definition.value("baseTableReference", json::object());
  return BaseTableDefinition{
      .base_table =
          {
              .project_id = base.value("projectId", ""),
              .dataset_id = base.value("datasetId", ""),
              .table_id = base.value("tableId", ""),
          },
      .time = definition.value(time, ""),
  };
}

}  // namespace

TableMetadata CommentMetadata(const std::optional<std::string>& comment) {
  const json object = ParseComment(comment);
  if (object.is_null()) {
    return {};
  }
  try {
    TableMetadata metadata = TableMetadataFromJson(object);
    metadata.label_order = object.value("labelOrder", std::vector<std::string>{});
    metadata.tag_order = object.value("tagOrder", std::vector<std::string>{});
    metadata.restored_from_snapshot = object.value("restoredFromSnapshot", false);
    metadata.clone = ParseBaseTableDefinition(object.value("cloneDefinition", json()), "cloneTime");
    metadata.snapshot =
        ParseBaseTableDefinition(object.value("snapshotDefinition", json()), "snapshotTime");
    if (object.contains("creationTime") && object["creationTime"].is_string()) {
      metadata.creation_time = std::stoll(object["creationTime"].get<std::string>());
    }
    return metadata;
  } catch (const ApiError&) {
    return {};
  }
}

std::string TableCommentStatement(const TableReference& table, TableMetadata metadata,
                                  const std::vector<FieldSchema>& schema) {
  ValidateLabels(metadata.labels);
  ValidateResourceTags(metadata.resource_tags);
  if (metadata.external_data) {
    if (metadata.external_data->uris.empty()) {
      throw ApiError::Invalid("External table source URIs must not be empty");
    }
    for (const auto& uri : metadata.external_data->uris) {
      if (!uri.starts_with("gs://")) {
        throw ApiError::Unsupported("external table sources other than gs://");
      }
      FindGcsWildcard(uri);
    }
    if (schema.empty()) {
      throw ApiError::Unsupported("external tables without an explicit schema");
    }
    if (metadata.partitioned() || metadata.require_partition_filter ||
        !metadata.clustering.empty() || !metadata.primary_key.empty() ||
        metadata.default_collation ||
        metadata.default_rounding_mode != RoundingMode::kUnspecified || metadata.clone ||
        metadata.snapshot) {
      throw ApiError::Unsupported("external table storage options");
    }
    const auto unsupported = std::ranges::find_if(schema, UnsupportedExternalField);
    if (unsupported != schema.end()) {
      throw ApiError::Unsupported("this external table column definition: " + unsupported->name);
    }
  }
  ValidatePartitioning(metadata, schema);
  metadata.primary_key = PrimaryKeyColumns(metadata.primary_key, schema);
  json comment = metadata.ToJson();
  if (!metadata.tag_order.empty()) {
    comment["tagOrder"] = metadata.tag_order;
  }
  if (!metadata.label_order.empty()) {
    comment["labelOrder"] = metadata.label_order;
  }
  if (metadata.restored_from_snapshot) {
    comment["restoredFromSnapshot"] = true;
  }
  return std::format("COMMENT ON TABLE {} IS {}", QualifiedName(table),
                     metadata.empty() ? "NULL" : QuoteLiteral(comment.dump()));
}

std::string ViewCommentStatement(const TableReference& table, const ViewMetadata& view) {
  if (view.metadata.external_data) {
    throw ApiError::Invalid("A view cannot have externalDataConfiguration");
  }
  ValidateLabels(view.metadata.labels);
  ValidateResourceTags(view.metadata.resource_tags);
  if (view.metadata.require_partition_filter) {
    throw ApiError::Unsupported("requirePartitionFilter on views");
  }
  // BigQuery keeps the primary key tables.patch gives a view, but rejects one tables.insert gives
  // it; what it does with one on a materialized view is not known.
  if (!view.metadata.primary_key.empty() && view.materialized) {
    throw ApiError::Unsupported("tableConstraints on materialized views");
  }
  if (view.metadata.partitioned() || !view.metadata.clustering.empty()) {
    if (view.materialized) {
      throw ApiError::Unsupported("partitioning or clustering of materialized views");
    }
    throw ApiError::Invalid("A view cannot be partitioned or clustered");
  }
  // A view has no columns of its own to give a default rounding mode, and what BigQuery does with
  // one is not known.
  if (view.metadata.default_rounding_mode != RoundingMode::kUnspecified) {
    throw ApiError::Unsupported("defaultRoundingMode on views");
  }
  ViewMetadata stored = view;
  stored.metadata.primary_key = PrimaryKeyColumns(view.metadata.primary_key, view.schema);
  json comment = stored.metadata.ToJson();
  if (!view.metadata.tag_order.empty()) {
    comment["tagOrder"] = view.metadata.tag_order;
  }
  if (!view.metadata.label_order.empty()) {
    comment["labelOrder"] = view.metadata.label_order;
  }
  if (view.materialized) {
    comment["materialized"] = true;
    if (view.materialized_source) {
      const auto& source = *view.materialized_source;
      comment["materializedSource"] = {
          {"projectId", source.table.project_id},
          {"datasetId", source.table.dataset_id},
          {"tableId", source.table.table_id},
          {"creationTime", source.creation_time},
      };
    }
  }
  comment["query"] = view.query;
  comment["fields"] = SchemaToJson(view.schema).at("fields");
  return std::format("COMMENT ON VIEW {} IS {}", QualifiedName(table),
                     QuoteLiteral(comment.dump()));
}

std::optional<ViewMetadata> ParseViewMetadata(const std::optional<std::string>& comment) {
  const json metadata = ParseComment(comment);
  if (!metadata.is_object() || !metadata.contains("query") || !metadata["query"].is_string() ||
      !metadata.contains("fields") || !metadata["fields"].is_array()) {
    return std::nullopt;
  }
  ViewMetadata view{
      .query = metadata["query"].get<std::string>(),
      .schema = {},
      .metadata = CommentMetadata(comment),
      .materialized = metadata.value("materialized", false),
  };
  try {
    if (view.materialized && metadata.contains("materializedSource")) {
      const auto& source = metadata["materializedSource"];
      view.materialized_source = MaterializedViewSource{
          .table =
              {
                  .project_id = source.at("projectId").get<std::string>(),
                  .dataset_id = source.at("datasetId").get<std::string>(),
                  .table_id = source.at("tableId").get<std::string>(),
              },
          .creation_time = source.at("creationTime").get<int64_t>(),
      };
    }
    for (const json& field : metadata["fields"]) {
      view.schema.push_back(FieldSchemaFromJson(field));
    }
  } catch (const ApiError&) {
    return std::nullopt;
  }
  return view;
}

}  // namespace bigquery_emulator_duckdb
