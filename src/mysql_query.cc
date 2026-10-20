#include "src/mysql_query.h"

#include <exception>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/ascii.h"
#include "duckdb.h"
#include "duckdb/main/capi/capi_internal.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"
#include "mysql.h"
#include "mysql_scanner_extension.hpp"
#include "mysql_utils.hpp"
#include "src/backend_error.h"
#include "src/field_schema.h"

namespace bigquery_emulator_duckdb {
namespace {

absl::StatusOr<FieldType> ColumnType(const MYSQL_FIELD& field, FieldType decimal_type) {
  switch (field.type) {
    case MYSQL_TYPE_TINY:
    case MYSQL_TYPE_SHORT:
    case MYSQL_TYPE_INT24:
    case MYSQL_TYPE_LONG:
    case MYSQL_TYPE_YEAR:
      return FieldType::kInteger;
    case MYSQL_TYPE_LONGLONG:
      // Unlike DECIMAL, UNSIGNED BIGINT is always NUMERIC, regardless of query options.
      return (field.flags & UNSIGNED_FLAG) != 0 ? FieldType::kNumeric : FieldType::kInteger;
    case MYSQL_TYPE_FLOAT:
    case MYSQL_TYPE_DOUBLE:
      return FieldType::kFloat;
    case MYSQL_TYPE_DECIMAL:
    case MYSQL_TYPE_NEWDECIMAL:
      return decimal_type;
    case MYSQL_TYPE_DATE:
      return FieldType::kDate;
    case MYSQL_TYPE_TIME:
      return FieldType::kTime;
    case MYSQL_TYPE_DATETIME:
      return FieldType::kDatetime;
    case MYSQL_TYPE_TIMESTAMP:
      return FieldType::kTimestamp;
    case MYSQL_TYPE_ENUM:
    case MYSQL_TYPE_SET:
      return FieldType::kString;
    case MYSQL_TYPE_STRING:
    case MYSQL_TYPE_VAR_STRING:
    case MYSQL_TYPE_VARCHAR:
    case MYSQL_TYPE_TINY_BLOB:
    case MYSQL_TYPE_MEDIUM_BLOB:
    case MYSQL_TYPE_LONG_BLOB:
    case MYSQL_TYPE_BLOB:
      return field.charsetnr == 63 ? FieldType::kBytes : FieldType::kString;
    case MYSQL_TYPE_BIT:
    case MYSQL_TYPE_GEOMETRY:
      return absl::InvalidArgumentError("Found unsupported MySQL type in BigQuery.");
    default:
      return absl::InvalidArgumentError("The emulator does not support EXTERNAL_QUERY MySQL type " +
                                        std::to_string(field.type));
  }
}

}  // namespace

void RegisterMySqlScanner(duckdb_database database) {
  try {
    duckdb::DuckDB& instance = *reinterpret_cast<duckdb::DatabaseWrapper*>(database)->database;
    instance.LoadStaticExtension<MysqlScannerExtension>();
    duckdb::Connection connection(instance);
    for (const char* setting : {
             "SET GLOBAL mysql_tinyint1_as_boolean = false",
             "SET GLOBAL mysql_bit1_as_boolean = false",
             "SET GLOBAL mysql_session_time_zone = '+00:00'",
         }) {
      const auto result = connection.Query(setting);
      if (result->HasError()) {
        throw BackendError(result->GetError());
      }
    }
  } catch (const std::exception& error) {
    throw BackendError(error.what());
  }
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): a connection and its SQL query.
absl::StatusOr<std::vector<FieldSchema>> DescribeMySqlQuery(const std::string& conninfo,
                                                            const std::string& query,
                                                            FieldType decimal_type) {
  try {
    const std::unique_ptr<MYSQL, decltype(&mysql_close)> connection(
        duckdb::MySQLUtils::Connect(conninfo, ""), mysql_close);
    // Prepare under the same time zone and character set as the scanner's execution sessions.
    if (mysql_query(connection.get(), "SET time_zone = '+00:00'") != 0) {
      return absl::InvalidArgumentError(mysql_error(connection.get()));
    }
    const std::unique_ptr<MYSQL_STMT, decltype(&mysql_stmt_close)> statement(
        mysql_stmt_init(connection.get()), mysql_stmt_close);
    if (!statement) {
      return absl::InvalidArgumentError(mysql_error(connection.get()));
    }
    if (mysql_stmt_prepare(statement.get(), query.data(), query.size()) != 0) {
      return absl::InvalidArgumentError(mysql_stmt_error(statement.get()));
    }
    const std::unique_ptr<MYSQL_RES, decltype(&mysql_free_result)> metadata(
        mysql_stmt_result_metadata(statement.get()), mysql_free_result);
    if (!metadata) {
      return absl::InvalidArgumentError(
          "The emulator does not support EXTERNAL_QUERY of a statement that returns no columns");
    }
    std::vector<FieldSchema> columns;
    std::set<std::string> names;
    const MYSQL_FIELD* fields = mysql_fetch_fields(metadata.get());
    for (unsigned int i = 0; i < mysql_num_fields(metadata.get()); ++i) {
      const auto type = ColumnType(fields[i], decimal_type);
      if (!type.ok()) {
        return type.status();
      }
      std::string name(fields[i].name, fields[i].name_length);
      if (!names.insert(absl::AsciiStrToLower(name)).second) {
        return absl::InvalidArgumentError(
            "The emulator does not support EXTERNAL_QUERY columns of the same name: " + name);
      }
      columns.push_back(FieldSchema{.name = std::move(name), .type = *type});
    }
    return columns;
  } catch (const std::exception& error) {
    return absl::InvalidArgumentError(error.what());
  }
}

}  // namespace bigquery_emulator_duckdb
