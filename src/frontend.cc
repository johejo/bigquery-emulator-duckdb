#include "src/frontend.h"

#include <stdexcept>
#include <string>
#include <utility>

#include "googlesql/public/options.pb.h"
#include "googlesql/public/parse_helpers.h"

namespace bigquery_emulator_duckdb {

FrontendResult::FrontendResult(std::string sql) : sql_(std::move(sql)) {}

FrontendResult ParseGoogleSql(const std::string& sql) {
  const auto status = googlesql::IsValidStatementSyntax(
      sql, googlesql::ErrorMessageMode::ERROR_MESSAGE_MULTI_LINE_WITH_CARET);
  if (!status.ok()) {
    throw std::runtime_error(status.ToString());
  }
  return FrontendResult(sql);
}

}  // namespace bigquery_emulator_duckdb
