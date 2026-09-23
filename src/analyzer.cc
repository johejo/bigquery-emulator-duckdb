#include "src/analyzer.h"

#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

#include "absl/status/status.h"
#include "absl/time/time.h"
#include "googlesql/public/analyzer.h"
#include "googlesql/public/analyzer_options.h"
#include "googlesql/public/analyzer_output.h"
#include "googlesql/public/error_helpers.h"
#include "googlesql/public/options.pb.h"
#include "src/frontend.h"

namespace bigquery_emulator_duckdb {

AnalyzerResult::AnalyzerResult(std::unique_ptr<const googlesql::AnalyzerOutput> analyzer_output)
    : analyzer_output_(std::move(analyzer_output)) {}

AnalyzerResult::AnalyzerResult(AnalyzerResult&&) noexcept = default;
AnalyzerResult& AnalyzerResult::operator=(AnalyzerResult&&) noexcept = default;
AnalyzerResult::~AnalyzerResult() = default;

const googlesql::ResolvedStatement& AnalyzerResult::statement() const {
  return *analyzer_output_->resolved_statement();
}

AnalyzerResult AnalyzeGoogleSql(const FrontendResult& frontend_result, googlesql::Catalog& catalog,
                                googlesql::TypeFactory& type_factory,
                                const AnalyzerSettings& settings) {
  if (!settings.named_parameters.empty() && !settings.positional_parameters.empty()) {
    throw std::invalid_argument("A statement cannot use both named and positional parameters");
  }

  googlesql::AnalyzerOptions options;
  options.set_language(GoogleSqlLanguageOptions());
  // BigQuery interprets civil times without an explicit zone as UTC.
  options.set_default_time_zone(absl::UTCTimeZone());
  options.set_error_message_mode(googlesql::ERROR_MESSAGE_MULTI_LINE_WITH_CARET);

  absl::Status status;
  if (!settings.positional_parameters.empty()) {
    options.set_parameter_mode(googlesql::PARAMETER_POSITIONAL);
    for (const googlesql::Type* type : settings.positional_parameters) {
      if (status.ok()) {
        status = options.AddPositionalQueryParameter(type);
      }
    }
  } else {
    for (const auto& [name, type] : settings.named_parameters) {
      if (status.ok()) {
        status = options.AddQueryParameter(name, type);
      }
    }
  }
  if (!status.ok()) {
    throw std::invalid_argument(status.ToString());
  }

  std::unique_ptr<const googlesql::AnalyzerOutput> analyzer_output;
  status = googlesql::AnalyzeStatementFromParserAST(frontend_result.statement(), options,
                                                    frontend_result.sql(), &catalog, &type_factory,
                                                    &analyzer_output);
  if (!status.ok()) {
    // In the caret mode the analyzer has already folded the location into the message; this
    // only matters for the few errors that still carry it as a payload.
    const googlesql::ErrorMessageOptions error_message_options = {
        .mode = googlesql::ErrorMessageMode::ERROR_MESSAGE_MULTI_LINE_WITH_CARET,
        .attach_error_location_payload = false,
    };
    throw std::runtime_error(
        googlesql::MaybeUpdateErrorFromPayload(error_message_options, frontend_result.sql(), status)
            .ToString());
  }
  return AnalyzerResult(std::move(analyzer_output));
}

}  // namespace bigquery_emulator_duckdb
