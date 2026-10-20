#include "src/column_metadata.h"

#include <algorithm>
#include <cstddef>
#include <format>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include "nlohmann/json.hpp"
#include "src/api_error.h"
#include "src/duckdb_sql.h"
#include "src/field_schema.h"
#include "src/references.h"

namespace bigquery_emulator_duckdb {

using nlohmann::json;

std::vector<std::string> ColumnCommentStatements(const TableReference& table,
                                                 const std::vector<FieldSchema>& schema) {
  CheckRoundingModes(schema);
  std::vector<std::string> statements;
  statements.reserve(schema.size());
  std::ranges::transform(schema, std::back_inserter(statements), [&](const FieldSchema& field) {
    return std::format("COMMENT ON COLUMN {}.{} IS {}", QualifiedName(table),
                       QuoteIdentifier(field.name), QuoteLiteral(field.ToJson().dump()));
  });
  return statements;
}

std::vector<std::string> RepeatedColumnDefaultStatements(const TableReference& table,
                                                         const std::vector<FieldSchema>& schema) {
  std::vector<std::string> statements;
  for (const FieldSchema& field : schema) {
    if (field.mode == FieldMode::kRepeated && field.default_value_expression.empty()) {
      statements.push_back(std::format("ALTER TABLE {} ALTER COLUMN {} SET DEFAULT []",
                                       QualifiedName(table), QuoteIdentifier(field.name)));
    }
  }
  return statements;
}

std::string ColumnCommentsQuery(const TableReference& table) {
  return std::format(
      "SELECT comment FROM duckdb_columns()"
      " WHERE database_name = {} AND schema_name = {} AND table_name = {} ORDER BY column_index",
      QuoteLiteral(table.project_id), QuoteLiteral(table.dataset_id), QuoteLiteral(table.table_id));
}

namespace {

// Numeric metadata must agree with the actual type: otherwise queries and REST readers
// could interpret the same stored number differently after an external schema change.
bool NumericStorageMismatch(const FieldSchema& recorded, const FieldSchema& stored) {
  const auto numeric = [](FieldType type) {
    return type == FieldType::kNumeric || type == FieldType::kBigNumeric;
  };
  if ((numeric(recorded.type) || numeric(stored.type)) && recorded.type != stored.type) {
    return true;
  }
  for (size_t i = 0; i < recorded.fields.size() && i < stored.fields.size(); ++i) {
    if (NumericStorageMismatch(recorded.fields.at(i), stored.fields.at(i))) {
      return true;
    }
  }
  return false;
}

}  // namespace

std::vector<FieldSchema> ApplyColumnComments(std::vector<FieldSchema> derived,
                                             const std::vector<json>& comments) {
  for (size_t i = 0; i < derived.size() && i < comments.size(); ++i) {
    if (!comments.at(i).is_string()) {
      continue;
    }
    const json value =
        json::parse(comments.at(i).get<std::string>(), nullptr, /*allow_exceptions=*/false);
    if (!value.is_object()) {
      continue;
    }
    FieldSchema field;
    try {
      field = FieldSchemaFromJson(value);
    } catch (const ApiError&) {
      // Not a TableFieldSchema, so not a comment the emulator wrote.
      continue;
    }
    if (ToLowerAscii(field.name) == ToLowerAscii(derived.at(i).name)) {
      if (NumericStorageMismatch(field, derived.at(i))) {
        throw ApiError::Invalid("Column " + field.name +
                                " metadata does not match its DuckDB numeric storage type");
      }
      derived.at(i) = std::move(field);
    }
  }
  return derived;
}

}  // namespace bigquery_emulator_duckdb
