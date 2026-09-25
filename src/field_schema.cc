#include "src/field_schema.h"

#include "nlohmann/json.hpp"

namespace bigquery_emulator_duckdb {

using nlohmann::json;

json FieldSchema::ToJson() const {
  json field = {{"name", name}, {"type", type}, {"mode", mode}};
  if (!fields.empty()) {
    field["fields"] = json::array();
    for (const FieldSchema& child : fields) {
      field["fields"].push_back(child.ToJson());
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

}  // namespace bigquery_emulator_duckdb
