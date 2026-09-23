#include "src/frontend.h"

#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

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

FrontendResult ParseGoogleSql(const std::string& sql) {
  // Some syntax BigQuery accepts is gated behind a language feature that the default options
  // leave off, QUALIFY among it, so every released feature is turned on. Accepting a little
  // more than BigQuery does is the lesser problem for an emulator: a query the parser rejects
  // cannot run at all.
  googlesql::LanguageOptions language_options;
  language_options.EnableMaximumLanguageFeatures();

  std::unique_ptr<googlesql::ParserOutput> parser_output;
  const absl::Status status =
      googlesql::ParseStatement(sql, googlesql::ParserOptions(language_options), &parser_output);
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

}  // namespace bigquery_emulator_duckdb
