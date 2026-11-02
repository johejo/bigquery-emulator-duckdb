#include "src/json_detect.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "nlohmann/json.hpp"
#include "src/api_error.h"
#include "src/duckdb_sql.h"
#include "src/field_schema.h"
#include "src/file_read.h"
#include "src/schema_detect.h"
#include "src/unordered_map_order.h"

// The rules below follow BigQuery's answers in
// tests/e2e/scenarios/testdata/external_table_json_autodetect.api.answers.json, which
// third_party/bigquery_docs/schema-detect.md describes only in outline.

namespace bigquery_emulator_duckdb {
namespace {

using nlohmann::ordered_json;

// What the sampled values of a field have shown so far.
struct Node {
  std::string name = {};
  bool repeated = false;
  bool record = false;
  // The type of its scalar values, absent while it has had only nulls and empty arrays.
  std::optional<DetectedKind> kind = std::nullopt;
  // In the order of their first appearance.
  std::vector<Node> fields = {};
};

void MergeObject(std::vector<Node>& fields, const ordered_json& object);

// `element` is a value of an array, whose own arrays BigQuery flattens when it detects the
// schema and rejects when it reads them.
void MergeValue(Node& node, const ordered_json& value, bool element = false) {
  if (value.is_null()) {
    return;
  }
  if (value.is_array()) {
    node.repeated = node.repeated || !element;
    for (const ordered_json& item : value) {
      MergeValue(node, item, true);
    }
    return;
  }
  if (value.is_object()) {
    node.record = true;
    MergeObject(node.fields, value);
    return;
  }
  DetectedKind kind = DetectedKind::kString;
  if (value.is_boolean()) {
    kind = DetectedKind::kBoolean;
  } else if (value.is_number_unsigned()) {
    kind = value.get<uint64_t>() > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())
               ? DetectedKind::kFloat
               : DetectedKind::kInteger;
  } else if (value.is_number_integer()) {
    kind = DetectedKind::kInteger;
  } else if (value.is_number_float()) {
    kind = DetectedKind::kFloat;
  } else {
    kind = DetectValue(value.get_ref<const std::string&>()).kind;
  }
  node.kind = node.kind ? MergeKinds(*node.kind, kind) : kind;
}

// Names differing only in case are different fields until the schema is checked.
void MergeObject(std::vector<Node>& fields, const ordered_json& object) {
  for (const auto& [name, value] : object.items()) {
    auto field = std::ranges::find(fields, name, &Node::name);
    if (field == fields.end()) {
      field = fields.insert(fields.end(), Node{.name = name});
    }
    MergeValue(*field, value);
  }
}

// The fields of `appeared`, in their order of first appearance, as BigQuery reports them: in the
// order a libc++ std::unordered_map of their names iterates them.
std::vector<FieldSchema> Fields(const std::vector<Node>& appeared, std::string_view table_id,
                                bool top) {
  std::vector<std::string> names;
  std::ranges::transform(appeared, std::back_inserter(names), &Node::name);
  std::vector<const Node*> ordered;
  std::ranges::transform(UnorderedMapOrder(names), std::back_inserter(ordered),
                         [&appeared](const std::string& name) {
                           return &*std::ranges::find(appeared, name, &Node::name);
                         });
  std::vector<FieldSchema> fields;
  for (const Node* node : ordered) {
    CheckDetectedFieldName(node->name);
    if (top) {
      CheckReservedFieldName(node->name);
    }
  }
  for (size_t i = 0; i < ordered.size(); ++i) {
    for (size_t j = 0; j < i; ++j) {
      if (ToLowerAscii(ordered.at(i)->name) == ToLowerAscii(ordered.at(j)->name)) {
        throw ApiError::InvalidQuery(
            std::format("Duplicate(Case Insensitive) field names: {} and {}. Table: {}",
                        ordered.at(j)->name, ordered.at(i)->name, table_id));
      }
    }
  }
  for (const Node* node : ordered) {
    FieldSchema& field = fields.emplace_back(FieldSchema{
        .name = node->name,
        .type = node->record ? FieldType::kRecord
                             : KindType(node->kind.value_or(DetectedKind::kString)),
        .mode = node->repeated ? FieldMode::kRepeated : FieldMode::kNullable,
    });
    if (node->record) {
      // BigQuery detects a RECORD with no fields, which DuckDB has no type for.
      if (node->fields.empty()) {
        throw ApiError::Unsupported("detecting the schema of an empty JSON object: " + node->name);
      }
      field.fields = Fields(node->fields, table_id, false);
    }
  }
  return fields;
}

}  // namespace

std::vector<FieldSchema> DetectJson(const FileSource& source, std::string_view table_id) {
  const std::string contents = ReadMaybeGzip(source.path);
  const std::string file = source.uri.substr(source.uri.rfind('/') + 1);
  std::vector<ordered_json> rows;
  for (size_t start = 0; start < contents.size();) {
    const size_t newline = contents.find('\n', start);
    const size_t end = newline == std::string::npos ? contents.size() : newline;
    const std::string_view line(contents.data() + start, end - start);
    start = end + 1;
    if (line.find_first_not_of(" \t\r") == std::string_view::npos) {
      continue;
    }
    ordered_json row = ordered_json::parse(line, nullptr, false);
    if (row.is_discarded()) {
      throw ApiError::Invalid(std::format(
          "Error while reading table: {}, error message: Failed to parse JSON: Unexpected end of "
          "string; Unexpected token; Could not parse value File: {}",
          table_id, file));
    }
    if (row.is_number()) {
      throw ApiError::Invalid(std::format(
          "Error while reading table: {}, error message: Failed to parse JSON: No active field "
          "found.; ParsedNumber returned false File: {}",
          table_id, file));
    }
    if (row.is_array()) {
      throw ApiError::Invalid(std::format(
          "Error while reading table: {}, error message: Failed to parse JSON: No object found "
          "when new array is started.; BeginArray returned false; Parser terminated before end "
          "of string File: {}",
          table_id, file));
    }
    if (!row.is_object()) {
      throw ApiError::Unsupported("detecting the schema of a JSON row that is not an object");
    }
    rows.push_back(std::move(row));
  }
  std::vector<Node> nodes;
  for (const ordered_json& row : rows) {
    MergeObject(nodes, row);
  }
  if (nodes.empty()) {
    throw ApiError::InvalidQuery(std::format("Schema has no fields. Table: {}", table_id));
  }
  return Fields(nodes, table_id, true);
}

}  // namespace bigquery_emulator_duckdb
