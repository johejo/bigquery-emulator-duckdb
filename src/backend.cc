#include "src/backend.h"

#include <memory>
#include <stdexcept>
#include <string>

#include "duckdb.hpp"

namespace bigquery_emulator_duckdb {

std::string ExecuteScalarString(const std::string& sql) {
  duckdb::DuckDB db(nullptr);
  duckdb::Connection connection(db);
  std::unique_ptr<duckdb::MaterializedQueryResult> result = connection.Query(sql);
  if (!result || result->HasError()) {
    throw std::runtime_error(result ? result->GetError() : "DuckDB query failed");
  }
  if (result->RowCount() == 0 || result->ColumnCount() == 0) {
    throw std::runtime_error("DuckDB query returned no scalar value");
  }
  return result->GetValue(0, 0).ToString();
}

}  // namespace bigquery_emulator_duckdb
