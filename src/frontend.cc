#include "src/frontend.h"

#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

#include "googlesql/parser/parse_tree.h"
#include "googlesql/parser/parser.h"
#include "googlesql/public/error_helpers.h"
#include "googlesql/public/language_options.h"
#include "googlesql/public/options.pb.h"

namespace bigquery_emulator_duckdb {

FrontendResult::FrontendResult(std::string sql,
                               std::unique_ptr<googlesql::ParserOutput> parser_output)
    : sql_(std::move(sql)), parser_output_(std::move(parser_output)) {}

FrontendResult::FrontendResult(FrontendResult&&) noexcept = default;
FrontendResult& FrontendResult::operator=(FrontendResult&&) noexcept = default;
FrontendResult::~FrontendResult() = default;

const googlesql::ASTStatement& FrontendResult::statement() const {
  return *parser_output_->statement();
}

const googlesql::LanguageOptions& GoogleSqlLanguageOptions() {
  static const googlesql::LanguageOptions* const kLanguageOptions = [] {
    auto* options = new googlesql::LanguageOptions();
    // Some syntax BigQuery accepts is gated behind a language feature that the default options
    // leave off, QUALIFY among it, so every released feature is turned on. Accepting a little
    // more than BigQuery does is the lesser problem for an emulator: a query the parser rejects
    // cannot run at all.
    options->EnableMaximumLanguageFeatures();
    // BigQuery is the external product: INT64 and FLOAT64 rather than the internal type set.
    options->set_product_mode(googlesql::PRODUCT_EXTERNAL);
    // The analyzer accepts only queries by default, but the emulator also runs DDL and DML.
    options->SetSupportsAllStatementKinds();
    return options;
  }();
  return *kLanguageOptions;
}

FrontendResult ParseGoogleSql(const std::string& sql) {
  std::unique_ptr<googlesql::ParserOutput> parser_output;
  const absl::Status status = googlesql::ParseStatement(
      sql, googlesql::ParserOptions(GoogleSqlLanguageOptions()), &parser_output);
  if (!status.ok()) {
    // The parser reports the error location as a payload; turn it into a readable message.
    const googlesql::ErrorMessageOptions error_message_options = {
        .mode = googlesql::ErrorMessageMode::ERROR_MESSAGE_MULTI_LINE_WITH_CARET,
        .attach_error_location_payload = false,
    };
    throw std::runtime_error(
        googlesql::MaybeUpdateErrorFromPayload(error_message_options, sql, status).ToString());
  }
  return {sql, std::move(parser_output)};
}

bool IsQueryOrDml(const FrontendResult& frontend_result) {
  const googlesql::ASTStatement& statement = frontend_result.statement();
  return statement.Is<googlesql::ASTQueryStatement>() ||
         statement.Is<googlesql::ASTInsertStatement>() ||
         statement.Is<googlesql::ASTUpdateStatement>() ||
         statement.Is<googlesql::ASTDeleteStatement>() ||
         statement.Is<googlesql::ASTMergeStatement>();
}

}  // namespace bigquery_emulator_duckdb
