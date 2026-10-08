#include "src/schema_sql.h"

#include <algorithm>
#include <format>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/statusor.h"
#include "googlesql/public/numeric_value.h"
#include "nlohmann/json.hpp"
#include "src/api_error.h"
#include "src/bignumeric.h"
#include "src/duckdb_sql.h"
#include "src/field_schema.h"
#include "src/references.h"
#include "src/type_mapping.h"

namespace bigquery_emulator_duckdb {
namespace {

using nlohmann::json;

std::string InsertRecord(const json& value, const FieldSchema& field, bool ignore_unknown_values) {
  if (!value.is_object()) {
    throw ApiError::Invalid("Expected an object for field " + field.name);
  }
  for (auto it = value.begin(); it != value.end(); ++it) {
    const bool known = std::ranges::any_of(
        field.fields, [&](const FieldSchema& child) { return child.name == it.key(); });
    if (!known && !ignore_unknown_values) {
      throw ApiError::Invalid("Unknown field: " + it.key());
    }
  }
  std::string fields;
  for (const FieldSchema& child : field.fields) {
    if (!fields.empty()) {
      fields += ", ";
    }
    const auto it = value.find(child.name);
    fields += QuoteIdentifier(child.name) + " := " +
              InsertValue(it == value.end() ? json(nullptr) : *it, child, ignore_unknown_values);
  }
  return "struct_pack(" + fields + ")";
}

// Appends to `statements` the statements of SchemaUpdateStatements for the fields `current` of
// a record of `table`. `path` is the DuckDB column path of the record, with a trailing dot, or
// empty for the table's columns, and `prefix` its BigQuery field path for errors.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the two paths of one record.
void AppendSchemaUpdateStatements(const TableReference& table, const std::string& path,
                                  const std::string& prefix,
                                  const std::vector<FieldSchema>& current,
                                  const std::vector<FieldSchema>& updated,
                                  std::vector<std::string>& statements) {
  const std::string mismatch =
      std::format("Provided Schema does not match Table {}.", TableName(table));
  for (size_t i = 0; i < current.size(); ++i) {
    const FieldSchema& before = current.at(i);
    const std::string name = prefix + before.name;
    if (i >= updated.size() || updated.at(i).name != before.name) {
      const bool kept = std::ranges::any_of(
          updated, [&](const FieldSchema& field) { return field.name == before.name; });
      throw ApiError::Invalid(std::format(
          "{} Field {} {}", mismatch, name,
          kept ? "has changed position; the emulator keeps existing fields in their order"
               : "is missing in new schema"));
    }
    const FieldSchema& after = updated.at(i);
    if (after.type != before.type) {
      throw ApiError::Invalid(std::format("{} Field {} has changed type from {} to {}", mismatch,
                                          name, FieldTypeName(before.type),
                                          FieldTypeName(after.type)));
    }
    if (after.max_length != before.max_length || after.precision != before.precision ||
        after.scale != before.scale) {
      throw ApiError::Invalid(
          std::format("{} Field {} has changed its type parameters", mismatch, name));
    }
    if (after.mode != before.mode &&
        (before.mode != FieldMode::kRequired || after.mode != FieldMode::kNullable)) {
      throw ApiError::Invalid(std::format("{} Field {} has changed mode from {} to {}", mismatch,
                                          name, FieldModeName(before.mode),
                                          FieldModeName(after.mode)));
    }
    if (after.default_value_expression != before.default_value_expression) {
      throw ApiError::Invalid("The emulator does not support changing the default value of field " +
                              name);
    }
    // DuckDB enforces REQUIRED only on columns; a field of a record keeps it in its metadata.
    if (after.mode != before.mode && path.empty()) {
      statements.push_back(std::format("ALTER TABLE {} ALTER COLUMN {} DROP NOT NULL",
                                       QualifiedName(table), QuoteIdentifier(before.name)));
    }
    if (before.type == FieldType::kRecord) {
      AppendSchemaUpdateStatements(table,
                                   path + QuoteIdentifier(before.name) +
                                       (before.mode == FieldMode::kRepeated ? ".element." : "."),
                                   name + ".", before.fields, after.fields, statements);
    }
  }
  for (size_t i = current.size(); i < updated.size(); ++i) {
    const FieldSchema& added = updated.at(i);
    if (added.mode == FieldMode::kRequired) {
      throw ApiError::Invalid(
          std::format("{} Cannot add required fields to an existing schema. (field: {}{})",
                      mismatch, prefix, added.name));
    }
    if (!added.default_value_expression.empty()) {
      throw ApiError::Invalid("The emulator does not support adding field " + prefix + added.name +
                              " with a default value");
    }
    // Existing rows read an added REPEATED column as empty, as rows that leave it out later do;
    // see RepeatedColumnDefaultStatements. DuckDB takes no default for a field of a record.
    const bool empty_default = path.empty() && added.mode == FieldMode::kRepeated;
    statements.push_back(std::format("ALTER TABLE {} ADD COLUMN {}{} {}{}", QualifiedName(table),
                                     path, QuoteIdentifier(added.name), ToDuckDbType(added),
                                     empty_default ? " DEFAULT []" : ""));
  }
}

}  // namespace

std::string ToDuckDbType(const FieldSchema& field) {
  absl::StatusOr<std::string> type = DuckDbColumnType(field);
  if (!type.ok()) {
    throw ApiError::Invalid(std::string(type.status().message()));
  }
  return *std::move(type);
}

std::string InsertValue(const json& value, const FieldSchema& field, bool ignore_unknown_values) {
  const std::string type = ToDuckDbType(field);
  if (value.is_null()) {
    return "CAST(NULL AS " + type + ")";
  }
  if (field.mode == FieldMode::kRepeated) {
    if (!value.is_array()) {
      throw ApiError::Invalid("Expected an array for field " + field.name);
    }
    FieldSchema element = field;
    element.mode = FieldMode::kNullable;
    std::string values;
    for (const json& item : value) {
      if (!values.empty()) {
        values += ", ";
      }
      values += InsertValue(item, element, ignore_unknown_values);
    }
    return "CAST([" + values + "] AS " + type + ")";
  }
  if (field.type == FieldType::kRecord) {
    return InsertRecord(value, field, ignore_unknown_values);
  }
  if (!value.is_primitive()) {
    throw ApiError::Invalid("Expected a scalar for field " + field.name);
  }
  const std::string scalar = value.is_string() ? value.get<std::string>() : value.dump();
  if (field.type == FieldType::kBytes) {
    return "from_base64(" + QuoteLiteral(scalar) + ")";
  }
  if (field.type == FieldType::kBigNumeric) {
    const auto number = googlesql::BigNumericValue::FromString(scalar);
    if (!number.ok()) {
      throw ApiError::Invalid("Invalid BIGNUMERIC value for field " + field.name + ": " + scalar);
    }
    return BigNumericSql(*number);
  }
  if (field.type == FieldType::kTimestamp && value.is_number()) {
    return "to_timestamp(CAST(" + QuoteLiteral(scalar) + " AS DOUBLE))";
  }
  return "CAST(" + QuoteLiteral(scalar) + " AS " + type + ")";
}

std::string ColumnDefinition(const FieldSchema& field) {
  return QuoteIdentifier(field.name) + " " + ToDuckDbType(field) +
         (field.mode == FieldMode::kRequired ? " NOT NULL" : "");
}

std::optional<std::string> ColumnDefinitions(const std::vector<FieldSchema>& schema) {
  std::string columns;
  for (const FieldSchema& field : schema) {
    if (!columns.empty()) {
      columns += ", ";
    }
    try {
      columns += ColumnDefinition(field);
    } catch (const ApiError&) {
      return std::nullopt;
    }
  }
  return columns;
}

std::vector<std::string> SchemaUpdateStatements(const TableReference& table,
                                                const std::vector<FieldSchema>& current,
                                                const std::vector<FieldSchema>& updated) {
  std::vector<std::string> statements;
  AppendSchemaUpdateStatements(table, "", "", current, updated, statements);
  return statements;
}

}  // namespace bigquery_emulator_duckdb
