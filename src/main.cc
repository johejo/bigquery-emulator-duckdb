#include <exception>
#include <iostream>
#include <string>

#include "src/backend.h"
#include "src/frontend.h"
#include "src/translator.h"

int main() {
  const std::string google_sql = "SELECT 1";
  try {
    const auto frontend_result = bigquery_emulator_duckdb::ParseGoogleSql(google_sql);
    const auto duckdb_sql = bigquery_emulator_duckdb::TranslateToDuckDbSql(frontend_result);
    std::cout << bigquery_emulator_duckdb::ExecuteScalarString(duckdb_sql) << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
