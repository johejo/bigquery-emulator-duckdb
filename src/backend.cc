#include "src/backend.h"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "duckdb.hpp"
#include "nlohmann/json.hpp"

namespace bigquery_emulator_duckdb {
namespace {

using nlohmann::json;

FieldSchema ToFieldSchema(const std::string& name, const duckdb::LogicalType& type);

// Maps a DuckDB type to the BigQuery type name used in TableFieldSchema.type.
std::string ToBigQueryTypeName(const duckdb::LogicalType& type) {
  switch (type.id()) {
    case duckdb::LogicalTypeId::BOOLEAN:
      return "BOOLEAN";
    case duckdb::LogicalTypeId::TINYINT:
    case duckdb::LogicalTypeId::SMALLINT:
    case duckdb::LogicalTypeId::INTEGER:
    case duckdb::LogicalTypeId::BIGINT:
    case duckdb::LogicalTypeId::UTINYINT:
    case duckdb::LogicalTypeId::USMALLINT:
    case duckdb::LogicalTypeId::UINTEGER:
    case duckdb::LogicalTypeId::UBIGINT:
      return "INTEGER";
    case duckdb::LogicalTypeId::HUGEINT:
    case duckdb::LogicalTypeId::UHUGEINT:
      return "BIGNUMERIC";
    case duckdb::LogicalTypeId::FLOAT:
    case duckdb::LogicalTypeId::DOUBLE:
      return "FLOAT";
    case duckdb::LogicalTypeId::DECIMAL:
      return duckdb::DecimalType::GetScale(type) <= 9 ? "NUMERIC" : "BIGNUMERIC";
    case duckdb::LogicalTypeId::VARCHAR:
    case duckdb::LogicalTypeId::UUID:
      return "STRING";
    case duckdb::LogicalTypeId::BLOB:
      return "BYTES";
    case duckdb::LogicalTypeId::DATE:
      return "DATE";
    case duckdb::LogicalTypeId::TIME:
    case duckdb::LogicalTypeId::TIME_TZ:
      return "TIME";
    // BigQuery TIMESTAMP is an absolute instant, which is DuckDB's TIMESTAMP WITH TIME ZONE.
    // BigQuery DATETIME is a civil time, which is DuckDB's plain TIMESTAMP.
    case duckdb::LogicalTypeId::TIMESTAMP_TZ:
      return "TIMESTAMP";
    case duckdb::LogicalTypeId::TIMESTAMP:
    case duckdb::LogicalTypeId::TIMESTAMP_SEC:
    case duckdb::LogicalTypeId::TIMESTAMP_MS:
    case duckdb::LogicalTypeId::TIMESTAMP_NS:
      return "DATETIME";
    case duckdb::LogicalTypeId::INTERVAL:
      return "INTERVAL";
    case duckdb::LogicalTypeId::STRUCT:
      return "RECORD";
    case duckdb::LogicalTypeId::LIST:
    case duckdb::LogicalTypeId::ARRAY:
      return ToBigQueryTypeName(duckdb::ListType::GetChildType(type));
    default:
      if (type.IsJSONType()) {
        return "JSON";
      }
      return "STRING";
  }
}

bool IsListLike(const duckdb::LogicalType& type) {
  return type.id() == duckdb::LogicalTypeId::LIST || type.id() == duckdb::LogicalTypeId::ARRAY;
}

const duckdb::LogicalType& ElementType(const duckdb::LogicalType& type) {
  return type.id() == duckdb::LogicalTypeId::ARRAY ? duckdb::ArrayType::GetChildType(type)
                                                   : duckdb::ListType::GetChildType(type);
}

FieldSchema ToFieldSchema(const std::string& name, const duckdb::LogicalType& type) {
  FieldSchema field;
  field.name = name;
  field.type = ToBigQueryTypeName(type);
  field.mode = IsListLike(type) ? "REPEATED" : "NULLABLE";
  const duckdb::LogicalType& scalar_type = IsListLike(type) ? ElementType(type) : type;
  if (scalar_type.id() == duckdb::LogicalTypeId::STRUCT) {
    for (const auto& [child_name, child_type] : duckdb::StructType::GetChildTypes(scalar_type)) {
      field.fields.push_back(ToFieldSchema(child_name, child_type));
    }
  }
  return field;
}

// BigQuery's default TIMESTAMP encoding: a decimal string of seconds since the Unix epoch.
std::string EpochSecondsString(int64_t micros) {
  const bool negative = micros < 0;
  const uint64_t magnitude =
      negative ? static_cast<uint64_t>(-(micros + 1)) + 1 : static_cast<uint64_t>(micros);
  const uint64_t seconds = magnitude / 1000000;
  const uint64_t fraction = magnitude % 1000000;
  std::string result = negative ? "-" : "";
  result += std::to_string(seconds);
  if (fraction != 0) {
    std::string digits = std::to_string(fraction);
    digits.insert(0, 6 - digits.size(), '0');
    while (digits.back() == '0') {
      digits.pop_back();
    }
    result += "." + digits;
  }
  return result;
}

json ToCell(const duckdb::Value& value);

// Encodes a scalar (non-list) DuckDB value as the "v" member of a BigQuery cell.
json ScalarToCellValue(const duckdb::Value& value) {
  if (value.IsNull()) {
    return nullptr;
  }
  const duckdb::LogicalType& type = value.type();
  switch (type.id()) {
    case duckdb::LogicalTypeId::BOOLEAN:
      return value.GetValue<bool>() ? "true" : "false";
    case duckdb::LogicalTypeId::BLOB:
      return duckdb::Blob::ToBase64(duckdb::string_t(duckdb::StringValue::Get(value)));
    case duckdb::LogicalTypeId::TIMESTAMP_TZ:
      // Kept as epoch microseconds, which is what clients asking for
      // formatOptions.useInt64Timestamp want; `TimestampsAsSeconds` produces the other
      // spelling for everyone else.
      return std::to_string(value.GetValueUnsafe<duckdb::timestamp_t>().value);
    case duckdb::LogicalTypeId::TIMESTAMP:
    case duckdb::LogicalTypeId::TIMESTAMP_SEC:
    case duckdb::LogicalTypeId::TIMESTAMP_MS:
    case duckdb::LogicalTypeId::TIMESTAMP_NS: {
      // DATETIME uses ISO 8601 with a "T" separator and no zone.
      std::string text = value.DefaultCastAs(duckdb::LogicalType::TIMESTAMP).ToString();
      if (text.size() > 10 && text[10] == ' ') {
        text[10] = 'T';
      }
      return text;
    }
    case duckdb::LogicalTypeId::STRUCT: {
      json fields = json::array();
      for (const duckdb::Value& child : duckdb::StructValue::GetChildren(value)) {
        fields.push_back(ToCell(child));
      }
      return json{{"f", std::move(fields)}};
    }
    default:
      return value.ToString();
  }
}

json ToCell(const duckdb::Value& value) {
  json cell;
  if (IsListLike(value.type())) {
    cell["v"] = json::array();
    if (!value.IsNull()) {
      for (const auto& element : duckdb::ListValue::GetChildren(value)) {
        cell["v"].push_back(json{{"v", ScalarToCellValue(element)}});
      }
    }
  } else {
    cell["v"] = ScalarToCellValue(value);
  }
  return cell;
}

// Statements that produce a result set. Everything else (DDL, DML, SET, ...) yields a
// single "Count" or "Success" column that is not part of the BigQuery result.
bool ProducesResultSet(duckdb::StatementType type) {
  return type == duckdb::StatementType::SELECT_STATEMENT ||
         type == duckdb::StatementType::EXPLAIN_STATEMENT ||
         type == duckdb::StatementType::PRAGMA_STATEMENT ||
         type == duckdb::StatementType::CALL_STATEMENT ||
         type == duckdb::StatementType::EXECUTE_STATEMENT;
}

bool IsDml(duckdb::StatementType type) {
  return type == duckdb::StatementType::INSERT_STATEMENT ||
         type == duckdb::StatementType::UPDATE_STATEMENT ||
         type == duckdb::StatementType::DELETE_STATEMENT ||
         type == duckdb::StatementType::MERGE_INTO_STATEMENT;
}

json CellsAsSeconds(const std::vector<FieldSchema>& schema, const json& cells);

// One value of a cell: the element of a REPEATED field, or the whole value of a scalar one.
json ValueAsSeconds(const FieldSchema& field, const json& value) {
  if (value.is_null()) {
    return value;
  }
  if (field.type == "TIMESTAMP") {
    return EpochSecondsString(std::stoll(value.get<std::string>()));
  }
  if (field.type == "RECORD") {
    return json{{"f", CellsAsSeconds(field.fields, value.at("f"))}};
  }
  return value;
}

json CellsAsSeconds(const std::vector<FieldSchema>& schema, const json& cells) {
  json result = json::array();
  for (size_t i = 0; i < schema.size() && i < cells.size(); ++i) {
    const FieldSchema& field = schema[i];
    const json& value = cells[i].at("v");
    if (field.mode != "REPEATED" || value.is_null()) {
      result.push_back(json{{"v", ValueAsSeconds(field, value)}});
      continue;
    }
    json elements = json::array();
    for (const json& element : value) {
      elements.push_back(json{{"v", ValueAsSeconds(field, element.at("v"))}});
    }
    result.push_back(json{{"v", std::move(elements)}});
  }
  return result;
}

void ThrowIfFailed(const std::unique_ptr<duckdb::MaterializedQueryResult>& result) {
  if (!result || result->HasError()) {
    throw BackendError(result ? result->GetError() : "DuckDB query failed");
  }
}

// Prepares a fresh connection: BigQuery evaluates timestamps in UTC, and `setup` (such as
// temporary functions) must precede the statement on the same connection.
void RunSetup(duckdb::Connection& connection, const std::vector<std::string>& setup) {
  ThrowIfFailed(connection.Query("SET TimeZone = 'UTC'"));
  for (const std::string& statement : setup) {
    ThrowIfFailed(connection.Query(statement));
  }
}

}  // namespace

json QueryResult::SchemaToJson() const { return bigquery_emulator_duckdb::SchemaToJson(schema); }

bool HasTimestampField(const std::vector<FieldSchema>& schema) {
  for (const FieldSchema& field : schema) {
    if (field.type == "TIMESTAMP" || HasTimestampField(field.fields)) {
      return true;
    }
  }
  return false;
}

json TimestampsAsSeconds(const std::vector<FieldSchema>& schema, const json& row) {
  return json{{"f", CellsAsSeconds(schema, row.at("f"))}};
}

Backend::Backend() : db_(std::make_unique<duckdb::DuckDB>(nullptr)) {}

Backend::~Backend() = default;

QueryResult Backend::Execute(const std::string& sql, const std::vector<std::string>& setup) {
  duckdb::Connection connection(*db_);
  RunSetup(connection, setup);
  std::unique_ptr<duckdb::MaterializedQueryResult> result = connection.Query(sql);
  ThrowIfFailed(result);

  QueryResult query_result;
  if (!ProducesResultSet(result->statement_type)) {
    if (IsDml(result->statement_type) && result->ColumnCount() == 1 && result->RowCount() > 0) {
      query_result.affected_rows = result->GetValue<int64_t>(0, 0);
    }
    return query_result;
  }
  query_result.has_rows = true;
  for (size_t i = 0; i < result->ColumnCount(); ++i) {
    query_result.schema.push_back(ToFieldSchema(result->names[i], result->types[i]));
  }
  for (size_t row = 0; row < result->RowCount(); ++row) {
    json cells = json::array();
    for (size_t column = 0; column < result->ColumnCount(); ++column) {
      cells.push_back(ToCell(result->GetValue(column, row)));
    }
    query_result.rows.push_back(json{{"f", std::move(cells)}});
  }
  return query_result;
}

QueryResult Backend::Prepare(const std::string& sql, const std::vector<std::string>& setup) {
  duckdb::Connection connection(*db_);
  RunSetup(connection, setup);
  // Preparing binds names and types without running the statement, which is what a dry run
  // needs: the query is validated and its result schema is known, but nothing is read or
  // written.
  std::unique_ptr<duckdb::PreparedStatement> prepared = connection.Prepare(sql);
  if (!prepared || prepared->HasError()) {
    throw BackendError(prepared ? prepared->GetError() : "DuckDB failed to prepare the query");
  }

  QueryResult query_result;
  if (!ProducesResultSet(prepared->GetStatementType())) {
    return query_result;
  }
  query_result.has_rows = true;
  const std::vector<std::string>& names = prepared->GetNames();
  const std::vector<duckdb::LogicalType>& types = prepared->GetTypes();
  for (size_t i = 0; i < names.size(); ++i) {
    query_result.schema.push_back(ToFieldSchema(names[i], types[i]));
  }
  return query_result;
}

std::vector<std::pair<size_t, std::string>> Backend::InsertRows(
    const std::vector<std::string>& statements, bool skip_invalid_rows) {
  duckdb::Connection connection(*db_);
  RunSetup(connection, {});
  std::vector<std::pair<size_t, std::string>> errors;
  if (!skip_invalid_rows) {
    ThrowIfFailed(connection.Query("BEGIN TRANSACTION"));
  }
  for (size_t i = 0; i < statements.size(); ++i) {
    std::unique_ptr<duckdb::MaterializedQueryResult> result = connection.Query(statements[i]);
    if (!result || result->HasError()) {
      errors.emplace_back(i, result ? result->GetError() : "DuckDB insert failed");
      if (!skip_invalid_rows) {
        ThrowIfFailed(connection.Query("ROLLBACK"));
        return errors;
      }
    }
  }
  if (!skip_invalid_rows) {
    ThrowIfFailed(connection.Query("COMMIT"));
  }
  return errors;
}

std::string ExecuteScalarString(const std::string& sql) {
  duckdb::DuckDB db(nullptr);
  duckdb::Connection connection(db);
  std::unique_ptr<duckdb::MaterializedQueryResult> result = connection.Query(sql);
  ThrowIfFailed(result);
  if (result->RowCount() == 0 || result->ColumnCount() == 0) {
    throw BackendError("DuckDB query returned no scalar value");
  }
  return result->GetValue(0, 0).ToString();
}

}  // namespace bigquery_emulator_duckdb
