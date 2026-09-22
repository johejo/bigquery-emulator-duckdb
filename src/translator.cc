#include "src/translator.h"

#include <string>

namespace bigquery_emulator_duckdb {

std::string TranslateToDuckDbSql(const FrontendResult& frontend_result) {
  (void)frontend_result;
  return "SELECT 1";
}

}  // namespace bigquery_emulator_duckdb
