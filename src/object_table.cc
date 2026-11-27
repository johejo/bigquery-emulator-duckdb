#include "src/object_table.h"

#include <algorithm>
#include <chrono>
#include <format>
#include <iterator>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "absl/strings/escaping.h"
#include "nlohmann/json.hpp"
#include "src/duckdb_sql.h"
#include "src/field_schema.h"
#include "src/gcs.h"
#include "src/object_ref.h"
#include "src/schema_sql.h"
#include "src/statement_resources.h"
#include "src/table_metadata.h"
#include "src/temporary_files.h"

namespace bigquery_emulator_duckdb {
namespace {

using nlohmann::json;

FieldSchema Field(std::string name, FieldType type, FieldMode mode = FieldMode::kNullable,
                  std::vector<FieldSchema> fields = {}) {
  return {.name = std::move(name), .type = type, .mode = mode, .fields = std::move(fields)};
}

// The values of a row for `object`, as tabledata.insertAll takes them.
json ObjectRow(const GcsObject& object, const GcsExternalData& definition) {
  const GcsObjectMetadata& metadata = object.metadata;
  json custom = json::array();
  for (const auto& [name, value] : metadata.custom) {
    custom.push_back({{"name", name}, {"value", value}});
  }
  const auto updated = std::chrono::floor<std::chrono::microseconds>(metadata.updated);
  // BigQuery gives the MD5 hash in hex, as in the example of
  // third_party/bigquery_docs/object-table-introduction.md, although its schema says base64.
  std::string md5;
  if (!absl::Base64Unescape(metadata.md5_hash, &md5)) {
    md5.clear();
  }
  return {
      {"uri", object.uri},
      {"generation", std::to_string(metadata.generation)},
      {"content_type", metadata.content_type.empty() ? json() : json(metadata.content_type)},
      {"size", std::to_string(metadata.size)},
      {"md5_hash", md5.empty() ? json() : json(absl::BytesToHexString(md5))},
      {"updated", std::format("{:%F %T}+00", updated)},
      {"metadata", std::move(custom)},
      {
          "ref",
          {
              {"uri", object.uri},
              {"version", std::to_string(metadata.generation)},
              {"authorizer", definition.connection_id ? json(*definition.connection_id) : json()},
              {"details", ObjectRefDetails(metadata, /*whole_seconds=*/false)},
          },
      },
  };
}

}  // namespace

std::vector<FieldSchema> ObjectTableSchema() {
  return {
      Field("uri", FieldType::kString),
      Field("generation", FieldType::kInteger),
      Field("content_type", FieldType::kString),
      Field("size", FieldType::kInteger),
      Field("md5_hash", FieldType::kString),
      Field("updated", FieldType::kTimestamp),
      Field("metadata", FieldType::kRecord, FieldMode::kRepeated,
            {Field("name", FieldType::kString), Field("value", FieldType::kString)}),
      Field("ref", FieldType::kRecord, FieldMode::kNullable,
            {
                Field("uri", FieldType::kString),
                Field("version", FieldType::kString),
                Field("authorizer", FieldType::kString),
                Field("details", FieldType::kJson),
            }),
  };
}

TableRead ObjectTableRead(GcsExternalData definition, std::vector<FieldSchema> schema,
                          GcsClient client) {
  return [definition = std::move(definition), schema = std::move(schema),
          client = std::move(client)](TableReadContext& context) mutable {
    std::vector<std::string> empty;
    std::ranges::transform(schema, std::back_inserter(empty), [](const FieldSchema& field) {
      return "CAST(NULL AS " + ToDuckDbType(field) + ") AS " + QuoteIdentifier(field.name);
    });
    empty.emplace_back("CAST(NULL AS BLOB) AS data");
    std::string none = "SELECT " + Join(empty, ", ") + " WHERE FALSE";
    // Only a statement that reads the pseudocolumn `data` downloads the objects.
    const bool data = !context.columns || context.columns->contains("data");
    if (context.dry_run) {
      return none;
    }
    std::map<std::string, GcsObject> objects;
    for (const std::string& uri : definition.uris) {
      std::vector<GcsObject> listed = client.List(uri);
      std::ranges::transform(listed, std::inserter(objects, objects.end()), [](GcsObject& object) {
        std::string key = object.uri;
        return std::pair(std::move(key), std::move(object));
      });
    }
    if (objects.empty()) {
      return none;
    }
    std::vector<std::string> rows;
    for (const auto& [uri, object] : objects) {
      const json row = ObjectRow(object, definition);
      std::vector<std::string> values;
      std::ranges::transform(schema, std::back_inserter(values), [&row](const FieldSchema& field) {
        return InsertValue(row.at(field.name), field, {}) + " AS " + QuoteIdentifier(field.name);
      });
      if (data) {
        const std::string path = context.files.Create();
        client.Download(uri, path);
        values.push_back("(SELECT content FROM read_blob(" + QuoteLiteral(path) + ")) AS data");
      } else {
        values.emplace_back("CAST(NULL AS BLOB) AS data");
      }
      rows.push_back("SELECT " + Join(values, ", "));
    }
    return Join(rows, " UNION ALL ");
  };
}

}  // namespace bigquery_emulator_duckdb
