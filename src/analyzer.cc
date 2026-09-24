#include "src/analyzer.h"

#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/time/time.h"
#include "googlesql/public/analyzer.h"
#include "googlesql/public/analyzer_options.h"
#include "googlesql/public/analyzer_output.h"
#include "googlesql/public/error_helpers.h"
#include "googlesql/public/options.pb.h"
#include "googlesql/public/strings.h"
#include "googlesql/resolved_ast/resolved_ast.h"
#include "src/catalog.h"
#include "src/field_schema.h"
#include "src/frontend.h"

namespace bigquery_emulator_duckdb {

AnalyzerResult::AnalyzerResult(std::unique_ptr<const googlesql::AnalyzerOutput> analyzer_output)
    : analyzer_output_(std::move(analyzer_output)) {}

AnalyzerResult::AnalyzerResult(AnalyzerResult&&) noexcept = default;
AnalyzerResult& AnalyzerResult::operator=(AnalyzerResult&&) noexcept = default;
AnalyzerResult::~AnalyzerResult() = default;

std::string ValueTableFieldName(const std::string& name, int index) {
  return name.empty() ? "_field_" + std::to_string(index + 1) : name;
}

const googlesql::ResolvedStatement& AnalyzerResult::statement() const {
  return *analyzer_output_->resolved_statement();
}

std::optional<std::vector<FieldSchema>> AnalyzerResult::result_schema() const {
  if (!statement().Is<googlesql::ResolvedQueryStmt>()) {
    return std::nullopt;
  }
  const auto* query = statement().GetAs<googlesql::ResolvedQueryStmt>();
  std::vector<FieldSchema> schema;
  if (query->is_value_table() && query->output_column_list_size() == 1 &&
      query->output_column_list(0)->column().type()->IsStruct()) {
    const auto& fields = query->output_column_list(0)->column().type()->AsStruct()->fields();
    for (size_t i = 0; i < fields.size(); ++i) {
      absl::StatusOr<FieldSchema> field = BigQueryFieldSchema(
          ValueTableFieldName(fields[i].name, static_cast<int>(i)), fields[i].type);
      if (!field.ok()) {
        return std::nullopt;
      }
      schema.push_back(*std::move(field));
    }
    return schema;
  }
  int anonymous_columns = 0;
  for (const auto& output_column : query->output_column_list()) {
    std::string name = output_column->name();
    if (googlesql::IsInternalAlias(name)) {
      name = "f" + std::to_string(anonymous_columns++) + "_";
    }
    absl::StatusOr<FieldSchema> field = BigQueryFieldSchema(name, output_column->column().type());
    if (!field.ok()) {
      return std::nullopt;
    }
    schema.push_back(*std::move(field));
  }
  return schema;
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
