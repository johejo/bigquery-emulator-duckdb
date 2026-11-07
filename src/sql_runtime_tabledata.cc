#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/time/time.h"
#include "nlohmann/json.hpp"
#include "src/api_error.h"
#include "src/backend.h"
#include "src/backend_error.h"
#include "src/duckdb_catalog.h"
#include "src/duckdb_sql.h"
#include "src/field_schema.h"
#include "src/function_catalog/catalog.h"
#include "src/references.h"
#include "src/schema_sql.h"
#include "src/sql_runtime.h"
#include "src/translator.h"

namespace bigquery_emulator_duckdb {

using nlohmann::json;

namespace {

// The DuckDB expression for `value`, a value of `field` in a streamed row. DuckDB's
// casts round halfway values away from zero, so the values of ROUND_HALF_EVEN fields are read
// unrounded and rounded to even, as BigQuery rounds the rows it streams.
std::string RoundedInsertValue(const function_catalog::Catalog& functions, const json& value,
                               const FieldSchema& field, bool ignore_unknown_values) {
  const FieldSchema unrounded = WithoutRoundingParameters(field);
  const std::optional<std::string> rounded = TranslateAppendedValue(
      functions, InsertValue(value, unrounded, ignore_unknown_values), unrounded, field);
  if (!rounded.has_value()) {
    throw ApiError::Unsupported("streaming into ROUND_HALF_EVEN columns");
  }
  return *rounded;
}

// Convert a returned default from REST cells to the named input values used for staged
// writes. Keep decimal strings and bytes intact and format timestamps without a float roundtrip.
json StorageDefaultValue(const json& value, const FieldSchema& field) {
  if (value.is_null()) {
    return nullptr;
  }
  if (field.mode == FieldMode::kRepeated) {
    json result = json::array();
    for (const auto& cell : value) {
      result.push_back(StorageDefaultValue(cell.at("v"), AsNullable(field)));
    }
    return result;
  }
  if (field.type == FieldType::kRecord) {
    json result = json::object();
    for (size_t i = 0; i < field.fields.size(); ++i) {
      result[field.fields.at(i).name] =
          StorageDefaultValue(value.at("f").at(i).at("v"), field.fields.at(i));
    }
    return result;
  }
  if (field.type == FieldType::kRange) {
    const std::string text = value.get<std::string>();
    const size_t separator = text.find(", ");
    const auto bound = [&](const std::string& part) -> json {
      return part == "UNBOUNDED" ? json(nullptr)
                                 : StorageDefaultValue(part, RangeBoundField(field, field.name));
    };
    return json{
        {"start", bound(text.substr(1, separator - 1))},
        {"end", bound(text.substr(separator + 2, text.size() - separator - 3))},
    };
  }
  if (field.type == FieldType::kTimestamp) {
    return absl::FormatTime("%Y-%m-%d %H:%M:%E6S%Ez",
                            absl::FromUnixMicros(std::stoll(value.get<std::string>())),
                            absl::UTCTimeZone());
  }
  return value;
}

}  // namespace

TableDataPage SqlRuntime::ListTableData(TableReference table, int64_t start_index,
                                        int64_t max_results, std::string_view selected_fields) {
  auto read = BeginTableRead(std::move(table));
  const TableInfo& info = read.info;
  if (info.materialized) {
    throw ApiError::Invalid("Cannot list a table of type MATERIALIZED_VIEW.");
  }
  if (info.view_query) {
    throw ApiError::Invalid("Cannot read a view with tabledata.list; use a query instead");
  }
  const std::string columns = selected_fields.empty()
                                  ? ReadColumns(info.schema)
                                  : ReadColumns(SelectFields(info.schema, selected_fields), true);
  try {
    const std::string relation = read.Relation();
    QueryResult result = read.session->Execute("SELECT " + columns + " FROM " + relation +
                                               " LIMIT " + std::to_string(max_results) +
                                               " OFFSET " + std::to_string(start_index));
    const auto count = read.session->Execute("SELECT count(*) FROM " + relation);
    const int64_t total_rows = count.rows.Int64(0, 0);
    read.session->Transaction("COMMIT");
    return {.result = std::move(result), .total_rows = total_rows};
  } catch (const BackendError& error) {
    throw ApiError::QueryFailure(error.what());
  }
}

std::vector<InsertError> SqlRuntime::WriteStorageRows(TableReference table,
                                                      const std::vector<FieldSchema>& schema,
                                                      json& rows, int64_t storage_id,
                                                      bool validate_only) {
  const std::scoped_lock expiration_lock(expiration_mutex_);
  table.project_id = ResolveProject(table.project_id);
  const TableInfo info = GetTable(table, false);
  table = info.reference;
  if (info.view_query) {
    throw ApiError::Invalid("Cannot write to a view: " + TableName(table));
  }
  RejectReadOnlyWrite(info);
  // Held until the transaction that keeps the table's rows and appends to it commits.
  std::unique_lock<std::mutex> kept;
  try {
    const auto session = backend_.NewSession();
    session->Transaction("BEGIN TRANSACTION");
    const QueryResult identity =
        session->Execute("SELECT table_oid FROM " +
                         RelationsQuery(table.project_id, table.dataset_id, table.table_id) +
                         " WHERE type = 'TABLE'");
    if (identity.rows.empty() || identity.rows.Int64(0, 0) != storage_id) {
      throw ApiError::NotFound("The Storage Write destination table was dropped or replaced");
    }
    CheckWritableTable(*session, table);
    if (SchemaToJson(TableSchema(*session, table)) != SchemaToJson(schema)) {
      throw ApiError::Invalid("Storage Write does not support destination schema changes");
    }
    if (!validate_only) {
      kept = std::unique_lock(HistoryMutex(table));
      (void)KeepVersion(*session, table);
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
          values.push_back(value == row.end()
                               ? "DEFAULT"
                               : RoundedInsertValue(functions_, *value, field, false));
        } catch (const ApiError& error) {
          return {{.index = index, .message = error.what()}};
        }
      }
      statements.push_back(std::format("INSERT INTO {} ({}) VALUES ({}){}", QualifiedName(table),
                                       Join(columns, ", "), Join(values, ", "),
                                       validate_only ? " RETURNING " + ReadColumns(schema) : ""));
    }
    for (size_t index = 0; index < statements.size(); ++index) {
      try {
        const QueryResult inserted = session->Execute(statements.at(index));
        if (validate_only) {
          auto& row = rows.at(index);
          const json returned = inserted.rows.at(0);
          for (size_t column = 0; column < schema.size(); ++column) {
            const auto& field = schema.at(column);
            if (!row.contains(field.name)) {
              row[field.name] = StorageDefaultValue(returned.at("f").at(column).at("v"), field);
            }
          }
        }
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

std::vector<InsertError> SqlRuntime::InsertTableData(TableReference table, const json& rows,
                                                     bool skip_invalid_rows,
                                                     bool ignore_unknown_values) {
  const std::scoped_lock expiration_lock(expiration_mutex_);
  table.project_id = ResolveProject(table.project_id);
  if (!rows.is_array()) {
    throw ApiError::Invalid("rows must be an array");
  }
  const TableInfo info = GetTable(table, false);
  table = info.reference;
  if (info.view_query) {
    throw ApiError::Invalid("Cannot insert into a view: " + TableName(table));
  }
  if (info.metadata.snapshot.has_value()) {
    throw ApiError::Invalid("Cannot add rows to a table of type SNAPSHOT.");
  }
  CheckWritableTable(backend_, table);
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
        literals.push_back(it == values.end()
                               ? "DEFAULT"
                               : RoundedInsertValue(functions_, *it, field, ignore_unknown_values));
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
    const std::unique_lock<std::mutex> kept =
        statements.empty() ? std::unique_lock<std::mutex>() : KeepVersion(backend_, table);
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
