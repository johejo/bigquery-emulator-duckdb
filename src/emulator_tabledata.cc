#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "nlohmann/json.hpp"
#include "src/api_error.h"
#include "src/backend.h"
#include "src/backend_error.h"
#include "src/duckdb_catalog.h"
#include "src/duckdb_sql.h"
#include "src/emulator.h"
#include "src/field_schema.h"
#include "src/references.h"
#include "src/schema_sql.h"
#include "src/translator.h"

namespace bigquery_emulator_duckdb {

using nlohmann::json;

namespace {

// The DuckDB expression for `value`, a value of `field` in a tabledata.insertAll row. DuckDB's
// casts round halfway values away from zero, so the values of ROUND_HALF_EVEN fields are read
// unrounded and rounded to even, as BigQuery rounds the rows it streams.
std::string RoundedInsertValue(const json& value, const FieldSchema& field,
                               bool ignore_unknown_values) {
  const FieldSchema unrounded = WithoutRoundingParameters(field);
  const std::optional<std::string> rounded = TranslateAppendedValue(
      InsertValue(value, unrounded, ignore_unknown_values), unrounded, field);
  if (!rounded.has_value()) {
    throw ApiError::Unsupported("tabledata.insertAll into ROUND_HALF_EVEN columns");
  }
  return *rounded;
}

}  // namespace

QueryResult Emulator::ListTableData(TableReference table, int64_t start_index,
                                    int64_t max_results) {
  table.project_id = ResolveProject(table.project_id);
  const TableInfo info = GetTable(table);
  if (info.view_query) {
    throw ApiError::Invalid("Cannot read a view with tabledata.list; use a query instead");
  }
  return Execute("SELECT " + ReadColumns(info.schema) + " FROM " + QualifiedName(table) +
                 " LIMIT " + std::to_string(max_results) + " OFFSET " +
                 std::to_string(start_index));
}

std::vector<InsertError> Emulator::WriteStorageRows(TableReference table,
                                                    const std::vector<FieldSchema>& schema,
                                                    const json& rows, int64_t storage_id,
                                                    bool validate_only) {
  table.project_id = ResolveProject(table.project_id);
  if (GetTable(table, false).view_query) {
    throw ApiError::Invalid("Cannot write to a view: " + TableName(table));
  }
  try {
    const auto session = backend_.NewSession();
    session->Transaction("BEGIN TRANSACTION");
    const QueryResult identity =
        session->Execute(std::format("SELECT table_oid FROM duckdb_tables() WHERE database_name = "
                                     "{} AND schema_name = {} AND table_name = {}",
                                     QuoteLiteral(table.project_id), QuoteLiteral(table.dataset_id),
                                     QuoteLiteral(table.table_id)));
    if (identity.rows.empty() || std::stoll(FirstColumnStrings(identity).at(0)) != storage_id) {
      throw ApiError::NotFound("The Storage Write destination table was dropped or replaced");
    }
    if (SchemaToJson(TableSchema(*session, table)) != SchemaToJson(schema)) {
      throw ApiError::Invalid("Storage Write does not support destination schema changes");
    }
    std::vector<std::string> statements;
    for (size_t index = 0; index < rows.size(); ++index) {
      const auto& row = rows.at(index);
      std::vector<std::string> columns;
      std::vector<std::string> values;
      for (const auto& field : schema) {
        columns.push_back(QuoteIdentifier(field.name));
        const auto value = row.find(field.name);
        try {
          values.push_back(InsertValue(value == row.end() ? json(nullptr) : *value, field, false));
        } catch (const ApiError& error) {
          return {{.index = index, .message = error.what()}};
        }
      }
      statements.push_back(std::format("INSERT INTO {} ({}) VALUES ({})", QualifiedName(table),
                                       Join(columns, ", "), Join(values, ", ")));
    }
    for (size_t index = 0; index < statements.size(); ++index) {
      try {
        session->Execute(statements.at(index));
      } catch (const BackendError& error) {
        return {{.index = index, .message = error.what()}};
      }
    }
    session->Transaction(validate_only ? "ROLLBACK" : "COMMIT");
    return {};
  } catch (const BackendError& error) {
    throw ApiError::Invalid(error.what());
  }
}

std::vector<InsertError> Emulator::InsertTableData(TableReference table, const json& rows,
                                                   bool skip_invalid_rows,
                                                   bool ignore_unknown_values) {
  table.project_id = ResolveProject(table.project_id);
  if (!rows.is_array()) {
    throw ApiError::Invalid("rows must be an array");
  }
  const TableInfo info = GetTable(table, false);
  if (info.view_query) {
    throw ApiError::Invalid("Cannot insert into a view: " + TableName(table));
  }
  const std::vector<FieldSchema>& schema = info.schema;
  std::vector<InsertError> errors;
  std::vector<std::string> statements;
  std::vector<size_t> indexes;
  for (size_t i = 0; i < rows.size(); ++i) {
    try {
      if (!rows[i].is_object() || !rows[i].contains("json") || !rows[i]["json"].is_object()) {
        throw ApiError::Invalid("Row must contain a json object");
      }
      const json& values = rows[i]["json"];
      for (auto it = values.begin(); it != values.end(); ++it) {
        const bool known = std::ranges::any_of(
            schema, [&](const FieldSchema& field) { return field.name == it.key(); });
        if (!known && !ignore_unknown_values) {
          throw ApiError::Invalid("Unknown field: " + it.key());
        }
      }
      std::vector<std::string> columns;
      std::vector<std::string> literals;
      for (const FieldSchema& field : schema) {
        columns.push_back(QuoteIdentifier(field.name));
        const auto it = values.find(field.name);
        literals.push_back(
            it == values.end() ? "DEFAULT" : RoundedInsertValue(*it, field, ignore_unknown_values));
      }
      std::string statement = "INSERT INTO ";
      statement += QualifiedName(table);
      statement += " (";
      statement += Join(columns, ", ");
      statement += ") VALUES (";
      statement += Join(literals, ", ");
      statement += ')';
      statements.push_back(std::move(statement));
      indexes.push_back(i);
    } catch (const ApiError& error) {
      errors.push_back({.index = i, .message = error.what()});
    }
  }
  if (!skip_invalid_rows && !errors.empty()) {
    return errors;
  }
  try {
    for (const auto& [index, message] : backend_.InsertRows(statements, skip_invalid_rows)) {
      errors.push_back({.index = indexes.at(index), .message = message});
    }
  } catch (const BackendError& error) {
    throw ApiError::Invalid(error.what());
  }
  std::ranges::sort(errors,
                    [](const InsertError& a, const InsertError& b) { return a.index < b.index; });
  return errors;
}

}  // namespace bigquery_emulator_duckdb
