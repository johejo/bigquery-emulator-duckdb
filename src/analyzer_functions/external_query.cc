#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "googlesql/public/function.h"
#include "googlesql/public/function_signature.h"
#include "googlesql/public/table_valued_function.h"
#include "googlesql/public/type.h"
#include "googlesql/public/value.h"
#include "src/analyzer_functions/functions.h"
#include "src/connection_id.h"
#include "src/field_schema.h"
#include "src/language_options.h"
#include "src/table_source.h"
#include "src/type_mapping.h"

namespace bigquery_emulator_duckdb::analyzer_functions {
namespace {

// The columns of EXTERNAL_QUERY, which the database it queries describes when the query is
// analyzed. Its arguments must be literals, since the query is prepared then.
absl::StatusOr<std::shared_ptr<googlesql::TVFSignature>> ExternalQuerySchema(
    TableSource& source, const std::string& default_project, googlesql::TypeFactory* type_factory,
    const std::vector<googlesql::TVFInputArgumentType>& arguments) {
  std::vector<std::string> literals;
  for (const auto& argument : arguments) {
    const auto scalar = argument.GetScalarArgType();
    if (!scalar.ok() || scalar->literal_value() == nullptr) {
      return absl::InvalidArgumentError(
          "The emulator does not support EXTERNAL_QUERY arguments that are not literals");
    }
    if (scalar->literal_value()->is_null()) {
      return absl::InvalidArgumentError("EXTERNAL_QUERY arguments must not be NULL");
    }
    literals.push_back(scalar->literal_value()->string_value());
  }
  const std::optional<std::string> key = ConnectionKey(literals.at(0), default_project);
  if (!key.has_value()) {
    return absl::InvalidArgumentError("Invalid connection ID: " + literals.at(0));
  }
  const absl::StatusOr<std::vector<FieldSchema>> columns = source.DescribeExternalQuery(
      *key, literals.at(1), literals.size() == 3 ? literals.at(2) : "{}");
  if (!columns.ok()) {
    return columns.status();
  }
  std::vector<googlesql::TVFRelation::Column> relation;
  for (const FieldSchema& column : *columns) {
    const absl::StatusOr<const googlesql::Type*> type = GoogleSqlType(column, type_factory);
    if (!type.ok()) {
      return type.status();
    }
    relation.emplace_back(column.name, *type);
  }
  return googlesql::TVFSignature::Create(arguments, googlesql::TVFRelation(std::move(relation)));
}

}  // namespace

std::unique_ptr<googlesql::TableValuedFunction> MakeExternalQueryFunction(
    TableSource& source, std::string default_project, googlesql::TypeFactory* type_factory) {
  const googlesql::FunctionArgumentType string(googlesql::types::StringType());
  return std::make_unique<googlesql::TableValuedFunction>(
      std::vector<std::string>{"external_query"}, kBigQueryGroup,
      std::vector<googlesql::FunctionSignature>{
          googlesql::FunctionSignature(
              googlesql::ARG_KIND_RELATION,
              {
                  string,
                  string,
                  googlesql::FunctionArgumentType(googlesql::types::StringType(),
                                                  googlesql::FunctionArgumentType::OPTIONAL),
              },
              /*context_id=*/int64_t{0}),
      },
      googlesql::TableValuedFunctionOptions().set_compute_result_type_callback(
          [&source, default_project = std::move(default_project), type_factory](
              googlesql::Catalog* /*catalog*/, googlesql::TypeFactory* /*type_factory*/,
              const googlesql::FunctionSignature& /*signature*/,
              const std::vector<googlesql::TVFInputArgumentType>& arguments,
              const googlesql::AnalyzerOptions& /*options*/) {
            return ExternalQuerySchema(source, default_project, type_factory, arguments);
          }));
}

}  // namespace bigquery_emulator_duckdb::analyzer_functions
