#pragma once

#include <memory>
#include <string>

namespace bigquery_emulator_duckdb {

class FrontendResult {
 public:
  explicit FrontendResult(std::string sql);

  const std::string& sql() const { return sql_; }

 private:
  std::string sql_;
};

FrontendResult ParseGoogleSql(const std::string& sql);

}  // namespace bigquery_emulator_duckdb
