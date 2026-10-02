#include "src/table_comments.h"

#include <format>
#include <optional>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"
#include "src/api_error.h"
#include "src/duckdb_sql.h"
#include "src/field_schema.h"
#include "src/references.h"
#include "src/table_metadata.h"

namespace bigquery_emulator_duckdb {
namespace {

using nlohmann::json;

// The JSON object a comment holds, or null for a comment the emulator did not write.
json ParseComment(const json& comment) {
  if (!comment.is_string()) {
    return nullptr;
  }
  json parsed = json::parse(comment.get<std::string>(), nullptr, /*allow_exceptions=*/false);
  return parsed.is_object() ? parsed : json(nullptr);
}

}  // namespace

TableMetadata CommentMetadata(const json& comment) {
  const json object = ParseComment(comment);
  if (object.is_null()) {
    return {};
  }
  try {
    return TableMetadataFromJson(object);
  } catch (const ApiError&) {
    return {};
  }
}

std::string TableCommentStatement(const TableReference& table, const TableMetadata& metadata,
                                  const std::vector<FieldSchema>& schema) {
  ValidateLabels(metadata.labels);
  ValidatePartitioning(metadata, schema);
  return std::format("COMMENT ON TABLE {} IS {}", QualifiedName(table),
                     metadata.empty() ? "NULL" : QuoteLiteral(metadata.ToJson().dump()));
}

std::string ViewCommentStatement(const TableReference& table, const ViewMetadata& view) {
  ValidateLabels(view.metadata.labels);
  if (view.metadata.partitioned() || !view.metadata.clustering.empty()) {
    throw ApiError::Invalid("A view cannot be partitioned or clustered");
  }
  json comment = view.metadata.ToJson();
  comment["query"] = view.query;
  comment["fields"] = SchemaToJson(view.schema).at("fields");
  return std::format("COMMENT ON VIEW {} IS {}", QualifiedName(table),
                     QuoteLiteral(comment.dump()));
}

std::optional<ViewMetadata> ParseViewMetadata(const json& comment) {
  const json metadata = ParseComment(comment);
  if (!metadata.is_object() || !metadata.contains("query") || !metadata["query"].is_string() ||
      !metadata.contains("fields") || !metadata["fields"].is_array()) {
    return std::nullopt;
  }
  ViewMetadata view{metadata["query"].get<std::string>(), {}, CommentMetadata(comment)};
  try {
    for (const json& field : metadata["fields"]) {
      view.schema.push_back(FieldSchemaFromJson(field));
    }
  } catch (const ApiError&) {
    return std::nullopt;
  }
  return view;
}

}  // namespace bigquery_emulator_duckdb
