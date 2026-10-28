#include "src/schema_sql.h"

#include <algorithm>
#include <cstddef>
#include <format>
#include <functional>
#include <iterator>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/statusor.h"
#include "googlesql/public/interval_value.h"
#include "googlesql/public/numeric_value.h"
#include "nlohmann/json.hpp"
#include "src/api_error.h"
#include "src/bignumeric.h"
#include "src/duckdb_sql.h"
#include "src/field_schema.h"
#include "src/interval.h"
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
  std::vector<std::string> fields;
  for (const FieldSchema& child : field.fields) {
    const auto it = value.find(child.name);
    fields.push_back(
        QuoteIdentifier(child.name) + " := " +
        InsertValue(it == value.end() ? json(nullptr) : *it, child, ignore_unknown_values));
  }
  return "struct_pack(" + Join(fields, ", ") + ")";
}

bool HasGeography(const std::vector<FieldSchema>& fields) {
  return std::ranges::any_of(fields, [](const FieldSchema& field) {
    return field.type == FieldType::kGeography || HasGeography(field.fields);
  });
}

bool HasWriteConstraints(const FieldSchema& field) {
  return field.mode == FieldMode::kRequired || field.max_length.has_value() ||
         std::ranges::any_of(field.fields, HasWriteConstraints);
}

// A predicate that checks REQUIRED and length constraints. CASE skips descendants of a
// NULL record. Positions also handle CTAS expressions whose field names differ from the schema.
std::string WriteCondition(const std::string& sql, const FieldSchema& field,
                           const std::string& path, const std::string& column, int depth,
                           bool assignment) {
  if (!HasWriteConstraints(field)) {
    return "true";
  }
  std::string condition = "true";
  if (field.mode == FieldMode::kRepeated) {
    const std::string name = "_write_element" + std::to_string(depth);
    condition = "coalesce(list_bool_and(list_transform(" + sql + ", lambda " + name + ": " +
                WriteCondition(name, AsNullable(field), path, column, depth + 1, assignment) +
                ")), true)";
  } else if (field.type == FieldType::kRecord) {
    std::vector<std::string> children;
    for (size_t i = 0; i < field.fields.size(); ++i) {
      const FieldSchema& child = field.fields.at(i);
      if (HasWriteConstraints(child)) {
        children.push_back(
            WriteCondition("struct_extract_at(" + sql + ", " + std::to_string(i + 1) + ")", child,
                           path + "." + child.name, column, depth, assignment));
      }
    }
    if (!children.empty()) {
      condition = "(" + Join(children, " AND ") + ")";
    }
  }
  if (field.mode != FieldMode::kRepeated && field.max_length.has_value()) {
    const std::string function =
        field.type == FieldType::kBytes ? "bq_check_bytes_length" : "bq_check_string_length";
    condition = function + "(" + sql + ", " + std::to_string(*field.max_length) + ", " +
                QuoteLiteral(assignment ? "" : column) + ")";
  }
  const std::string missing =
      field.mode == FieldMode::kRequired
          ? "error(" +
                QuoteLiteral("Required field " + path + " cannot be null; error in writing field " +
                             column) +
                ")"
          : "true";
  return "CASE WHEN " + sql + " IS NULL THEN " + missing + " ELSE " + condition + " END";
}

// The DuckDB value `sql` of `field` with each GEOGRAPHY in it, at any depth, replaced by
// `geography(value)`.
std::string MapGeographies(const std::string& sql, const FieldSchema& field, int depth,
                           const std::function<std::string(const std::string&)>& geography) {
  if (!HasGeography({field})) {
    return sql;
  }
  if (field.mode == FieldMode::kRepeated) {
    return MapElements(sql, field, depth,
                       [&](const std::string& name, const FieldSchema& element, int element_depth) {
                         return MapGeographies(name, element, element_depth, geography);
                       });
  }
  if (field.type == FieldType::kRecord) {
    return MapFields(sql, field, [&](const std::string& value, const FieldSchema& child) {
      return MapGeographies(value, child, depth, geography);
    });
  }
  return geography(sql);
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
    if (after.range_element_type != before.range_element_type) {
      throw ApiError::Invalid(
          std::format("{} Field {} has changed its rangeElementType", mismatch, name));
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
      throw ApiError::Unsupported("changing the default value of field " + name);
    }
    if (after.collation != before.collation) {
      throw ApiError::Invalid(std::format(
          "{} Invalid schema update. Updating field collation is not supported. Field {} has "
          "changed collation from \"{}\" to \"{}\"",
          mismatch, name, before.collation.value_or(""), after.collation.value_or("")));
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
      throw ApiError::Unsupported("adding field " + prefix + added.name + " with a default value");
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

std::string RangeFromText(const std::string& text, const FieldSchema& field) {
  const std::string element = ToDuckDbType(RangeBoundField(field, field.name));
  return std::format(
      "list_transform([bq_range_bounds({}, false)], lambda _b: CASE WHEN _b IS NULL THEN NULL ELSE "
      "CAST({} AS {}) END)[1]",
      text, DuckDbRange("CAST(_b[1] AS " + element + ")", "CAST(_b[2] AS " + element + ")"),
      ToDuckDbType(field));
}

std::string InsertValue(const json& value, const FieldSchema& field, bool ignore_unknown_values) {
  const std::string type = ToDuckDbType(field);
  if (value.is_null()) {
    if (field.mode == FieldMode::kRequired) {
      throw ApiError::Invalid("Required field " + field.name + " cannot be null");
    }
    return "CAST(NULL AS " + type + ")";
  }
  if (field.mode == FieldMode::kRepeated) {
    if (!value.is_array()) {
      throw ApiError::Invalid("Expected an array for field " + field.name);
    }
    const FieldSchema element = AsNullable(field);
    std::vector<std::string> values;
    std::ranges::transform(value, std::back_inserter(values), [&](const json& item) {
      return InsertValue(item, element, ignore_unknown_values);
    });
    return "CAST([" + Join(values, ", ") + "] AS " + type + ")";
  }
  if (field.type == FieldType::kRecord) {
    return InsertRecord(value, field, ignore_unknown_values);
  }
  if (field.type == FieldType::kRange) {
    // {"start": ..., "end": ...}, where a missing or null bound is unbounded.
    if (!value.is_object() || !field.range_element_type.has_value()) {
      throw ApiError::Invalid("Expected an object for field " + field.name);
    }
    std::vector<std::string> bounds;
    for (const char* name : {"start", "end"}) {
      const auto it = value.find(name);
      bounds.push_back(InsertValue(it == value.end() ? json(nullptr) : *it,
                                   RangeBoundField(field, field.name), false));
    }
    return "CAST(" + DuckDbRange(bounds.at(0), bounds.at(1)) + " AS " + type + ")";
  }
  if (!value.is_primitive()) {
    throw ApiError::Invalid("Expected a scalar for field " + field.name);
  }
  const std::string scalar = value.is_string() ? value.get<std::string>() : value.dump();
  if (field.type == FieldType::kBytes) {
    return "from_base64(" + QuoteLiteral(scalar) + ")";
  }
  if (field.type == FieldType::kGeography) {
    return StoredValue(QuoteLiteral(scalar), field);
  }
  if (field.type == FieldType::kBigNumeric) {
    const auto number = googlesql::BigNumericValue::FromString(scalar);
    if (!number.ok()) {
      throw ApiError::Invalid("Invalid BIGNUMERIC value for field " + field.name + ": " + scalar);
    }
    return BigNumericSql(*number);
  }
  if (field.type == FieldType::kInterval) {
    // GoogleSQL parses the text as it does a query parameter's.
    const auto interval = googlesql::IntervalValue::ParseFromString(scalar, /*allow_nanos=*/false);
    if (!interval.ok()) {
      throw ApiError::Invalid("Invalid INTERVAL value for field " + field.name + ": " + scalar);
    }
    return IntervalSql(*interval);
  }
  if (field.type == FieldType::kTimestamp && value.is_number()) {
    return "to_timestamp(CAST(" + QuoteLiteral(scalar) + " AS DOUBLE))";
  }
  return "CAST(" + QuoteLiteral(scalar) + " AS " + type + ")";
}

std::string CheckedWriteValue(const std::string& sql, const FieldSchema& field, bool assignment) {
  if (!HasWriteConstraints(field)) {
    return sql;
  }
  const std::string name = "_write_value";
  return "list_transform([" + sql + "], lambda " + name + ": CASE WHEN " +
         WriteCondition(name, field, field.name, field.name, 0, assignment) + " THEN " + name +
         " END)[1]";
}

std::string StoredValue(const std::string& sql, const FieldSchema& field) {
  return MapGeographies(CheckedWriteValue(sql, field), field, 0, [&](const std::string& value) {
    return "bq_geography_input(" + value + ")";
  });
}

namespace {

// The DuckDB value `sql` of `field` as ReadColumns reads it: each GEOGRAPHY in it as WKT and,
// when `selected` is set, only the fields `field` keeps of each RECORD in it.
std::string ReadValue(const std::string& sql, const FieldSchema& field, int depth, bool selected) {
  if (!selected || field.type != FieldType::kRecord) {
    return MapGeographies(sql, field, depth,
                          [](const std::string& value) { return "bq_st_astext(" + value + ")"; });
  }
  if (field.mode == FieldMode::kRepeated) {
    return MapElements(sql, field, depth,
                       [](const std::string& name, const FieldSchema& element, int element_depth) {
                         return ReadValue(name, element, element_depth, true);
                       });
  }
  return MapFields(sql, field, [&](const std::string& value, const FieldSchema& child) {
    return ReadValue(value, child, depth, true);
  });
}

}  // namespace

std::string ReadColumns(const std::vector<FieldSchema>& schema, bool selected) {
  if (!selected && !HasGeography(schema)) {
    return "*";
  }
  std::vector<std::string> columns;
  for (const FieldSchema& field : schema) {
    const std::string name = QuoteIdentifier(field.name);
    columns.push_back(std::format("{} AS {}", ReadValue(name, field, 0, selected), name));
  }
  return Join(columns, ", ");
}

std::string ColumnDefinition(const FieldSchema& field) {
  return QuoteIdentifier(field.name) + " " + ToDuckDbType(field) +
         (field.mode == FieldMode::kRequired ? " NOT NULL" : "");
}

std::optional<std::string> ColumnDefinitions(const std::vector<FieldSchema>& schema) {
  std::vector<std::string> columns;
  for (const FieldSchema& field : schema) {
    try {
      columns.push_back(ColumnDefinition(field));
    } catch (const ApiError&) {
      return std::nullopt;
    }
  }
  return Join(columns, ", ");
}

std::string MapElements(
    const std::string& sql, const FieldSchema& field, int depth,
    const std::function<std::string(const std::string&, const FieldSchema&, int)>& element) {
  const std::string name = "_e" + std::to_string(depth);
  return std::format("list_transform({}, lambda {}: {})", sql, name,
                     element(name, AsNullable(field), depth + 1));
}

std::string MapFields(
    const std::string& sql, const FieldSchema& field,
    const std::function<std::string(const std::string&, const FieldSchema&)>& value) {
  std::vector<std::string> fields;
  std::ranges::transform(field.fields, std::back_inserter(fields), [&](const FieldSchema& child) {
    return std::format(
        "{} := {}", QuoteIdentifier(child.name),
        value(std::format("struct_extract({}, {})", sql, QuoteLiteral(child.name)), child));
  });
  return std::format("CASE WHEN {0} IS NULL THEN NULL ELSE struct_pack({1}) END", sql,
                     Join(fields, ", "));
}

std::vector<std::string> SchemaUpdateStatements(const TableReference& table,
                                                const std::vector<FieldSchema>& current,
                                                const std::vector<FieldSchema>& updated) {
  std::vector<std::string> statements;
  AppendSchemaUpdateStatements(table, "", "", current, updated, statements);
  return statements;
}

}  // namespace bigquery_emulator_duckdb
