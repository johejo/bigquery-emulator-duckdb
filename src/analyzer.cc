#include "src/analyzer.h"

#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/time/time.h"
#include "googlesql/base/status_macros.h"
#include "googlesql/public/analyzer.h"
#include "googlesql/public/analyzer_options.h"
#include "googlesql/public/analyzer_output.h"
#include "googlesql/public/error_helpers.h"
#include "googlesql/public/options.pb.h"
#include "googlesql/public/strings.h"
#include "googlesql/resolved_ast/resolved_ast.h"
#include "googlesql/scripting/error_helpers.h"
#include "googlesql/scripting/script_executor.h"
#include "googlesql/scripting/script_segment.h"
#include "src/catalog.h"
#include "src/field_schema.h"

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

const googlesql::ResolvedExpr& AnalyzerResult::expression() const {
  return *analyzer_output_->resolved_expr();
}

std::optional<std::vector<FieldSchema>> AnalyzerResult::result_schema() const {
  return ResultSchema(statement());
}

std::optional<std::vector<FieldSchema>> ResultSchema(
    const googlesql::ResolvedStatement& statement) {
  if (!statement.Is<googlesql::ResolvedQueryStmt>()) {
    return std::nullopt;
  }
  const auto* query = statement.GetAs<googlesql::ResolvedQueryStmt>();
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

namespace {

// The options a statement of a request, or of the script `settings.script`, is analyzed with.
absl::StatusOr<googlesql::AnalyzerOptions> AnalyzerOptions(const AnalyzerSettings& settings,
                                                           googlesql::ErrorMessageMode mode) {
  googlesql::AnalyzerOptions options;
  options.set_language(GoogleSqlLanguageOptions());
  // BigQuery interprets civil times without an explicit zone as UTC.
  options.set_default_time_zone(absl::UTCTimeZone());
  options.set_error_message_mode(mode);
  if (!settings.positional_parameters.empty()) {
    options.set_parameter_mode(googlesql::PARAMETER_POSITIONAL);
    for (const googlesql::Type* type : settings.positional_parameters) {
      GOOGLESQL_RETURN_IF_ERROR(options.AddPositionalQueryParameter(type));
    }
  } else {
    for (const auto& [name, type] : settings.named_parameters) {
      GOOGLESQL_RETURN_IF_ERROR(options.AddQueryParameter(name, type));
    }
  }
  if (settings.script != nullptr) {
    GOOGLESQL_RETURN_IF_ERROR(settings.script->UpdateAnalyzerOptions(options));
  }
  return options;
}

}  // namespace

AnalyzerResult AnalyzeGoogleSql(const std::string& sql, googlesql::Catalog& catalog,
                                googlesql::TypeFactory& type_factory,
                                const AnalyzerSettings& settings) {
  if (!settings.named_parameters.empty() && !settings.positional_parameters.empty()) {
    throw std::invalid_argument("A statement cannot use both named and positional parameters");
  }
  absl::StatusOr<googlesql::AnalyzerOptions> options =
      AnalyzerOptions(settings, googlesql::ERROR_MESSAGE_MULTI_LINE_WITH_CARET);
  if (!options.ok()) {
    throw std::invalid_argument(options.status().ToString());
  }

  std::unique_ptr<const googlesql::AnalyzerOutput> analyzer_output;
  const absl::Status status =
      googlesql::AnalyzeStatement(sql, *options, &catalog, &type_factory, &analyzer_output);
  if (!status.ok()) {
    // In the caret mode the analyzer has already folded the location into the message; this
    // only matters for the few errors that still carry it as a payload.
    const googlesql::ErrorMessageOptions error_message_options = {
        .mode = googlesql::ErrorMessageMode::ERROR_MESSAGE_MULTI_LINE_WITH_CARET,
        .attach_error_location_payload = false,
    };
    throw std::runtime_error(
        googlesql::MaybeUpdateErrorFromPayload(error_message_options, sql, status).ToString());
  }
  return AnalyzerResult(std::move(analyzer_output));
}

absl::StatusOr<googlesql::AnalyzerOptions> ScriptAnalyzerOptions(const AnalyzerSettings& settings) {
  return AnalyzerOptions(settings, googlesql::ERROR_MESSAGE_MULTI_LINE_WITH_CARET);
}

absl::StatusOr<googlesql::TypeWithParameters> AnalyzeScriptType(
    const googlesql::ScriptSegment& segment, googlesql::Catalog& catalog,
    googlesql::TypeFactory& type_factory, const AnalyzerSettings& settings) {
  GOOGLESQL_ASSIGN_OR_RETURN(googlesql::AnalyzerOptions options,
                             AnalyzerOptions(settings, googlesql::ERROR_MESSAGE_WITH_PAYLOAD));
  googlesql::TypeWithParameters type;
  googlesql::TypeModifiers modifiers;
  GOOGLESQL_RETURN_IF_ERROR(googlesql::AnalyzeType(std::string(segment.GetSegmentText()), options,
                                                   &catalog, &type_factory, &type.type, &modifiers))
      .With(googlesql::ConvertLocalErrorToScriptError(segment));
  type.type_params = modifiers.type_parameters();
  return type;
}

absl::StatusOr<AnalyzerResult> AnalyzeScriptStatement(const googlesql::ScriptSegment& segment,
                                                      googlesql::Catalog& catalog,
                                                      googlesql::TypeFactory& type_factory,
                                                      const AnalyzerSettings& settings) {
  GOOGLESQL_ASSIGN_OR_RETURN(googlesql::AnalyzerOptions options,
                             AnalyzerOptions(settings, googlesql::ERROR_MESSAGE_WITH_PAYLOAD));
  std::unique_ptr<const googlesql::AnalyzerOutput> analyzer_output;
  GOOGLESQL_RETURN_IF_ERROR(googlesql::AnalyzeStatement(segment.GetSegmentText(), options, &catalog,
                                                        &type_factory, &analyzer_output))
      .With(googlesql::ConvertLocalErrorToScriptError(segment));
  return AnalyzerResult(std::move(analyzer_output));
}

absl::StatusOr<AnalyzerResult> AnalyzeScriptExpression(std::string_view sql,
                                                       const googlesql::ScriptSegment* segment,
                                                       const googlesql::Type* target_type,
                                                       googlesql::Catalog& catalog,
                                                       googlesql::TypeFactory& type_factory,
                                                       const AnalyzerSettings& settings) {
  GOOGLESQL_ASSIGN_OR_RETURN(
      googlesql::AnalyzerOptions options,
      AnalyzerOptions(settings, segment != nullptr ? googlesql::ERROR_MESSAGE_WITH_PAYLOAD
                                                   : googlesql::ERROR_MESSAGE_ONE_LINE));
  std::unique_ptr<const googlesql::AnalyzerOutput> analyzer_output;
  const absl::Status status =
      target_type != nullptr
          ? googlesql::AnalyzeExpressionForAssignmentToType(
                sql, options, &catalog, &type_factory, target_type,
                /*target_type_modifiers=*/std::nullopt, &analyzer_output)
          : googlesql::AnalyzeExpression(sql, options, &catalog, &type_factory, &analyzer_output);
  if (segment != nullptr) {
    GOOGLESQL_RETURN_IF_ERROR(status).With(googlesql::ConvertLocalErrorToScriptError(*segment));
  }
  GOOGLESQL_RETURN_IF_ERROR(status);
  return AnalyzerResult(std::move(analyzer_output));
}

}  // namespace bigquery_emulator_duckdb
