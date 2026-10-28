#include "src/translator.h"

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "absl/status/statusor.h"
#include "absl/strings/match.h"
#include "googlesql/public/type.h"
#include "googlesql/public/value.h"
#include "src/analyzer.h"
#include "src/default_dataset.h"
#include "src/field_schema.h"
#include "src/function_catalog/catalog.h"
#include "src/query_parameters.h"
#include "src/schema_sql.h"
#include "src/system_variables.h"
#include "src/translated_statement.h"
#include "src/translator/collation.h"
#include "src/translator/context.h"
#include "src/translator/expression.h"
#include "src/translator/function.h"
#include "src/translator/literal.h"
#include "src/translator/statement.h"
#include "src/type_mapping.h"

namespace bigquery_emulator_duckdb {

std::optional<TranslatedStatement> TranslateStatement(
    const function_catalog::Catalog& functions, const googlesql::ResolvedStatement& statement,
    const QueryParameters& parameters, const DefaultDataset& defaults, std::string* unsupported,
    const googlesql::SystemVariableValuesMap* system_variables) {
  TranslatedStatement translated;
  translator::Context context{
      .functions = functions,
      .parameters = parameters,
      .defaults = defaults,
      .statement = translated,
      .system_variables = system_variables,
  };
  auto sql = translator::Statement(statement, translator::Scope{.context = context});
  if (!sql) {
    if (unsupported != nullptr) {
      *unsupported = context.unsupported.empty() ? "unsupported construct" : context.unsupported;
    }
    return std::nullopt;
  }
  translated.sql = *std::move(sql);
  translated.statement_type = translator::StatementType(statement);
  translated.result_schema = ResultSchema(statement);
  translated.result_has_collation = translator::ResultHasCollation(statement);
  return translated;
}

std::optional<std::string> TranslateExpression(
    const function_catalog::Catalog& functions, const googlesql::ResolvedExpr& expression,
    const QueryParameters& parameters, const DefaultDataset& defaults, std::string* unsupported,
    const googlesql::SystemVariableValuesMap* system_variables) {
  // Context takes a statement, though an expression records nothing in it.
  TranslatedStatement translated;
  translator::Context context{
      .functions = functions,
      .parameters = parameters,
      .defaults = defaults,
      .statement = translated,
      .system_variables = system_variables,
  };
  auto sql = translator::Expression(expression, translator::Scope{.context = context}, {});
  if (!sql) {
    if (unsupported != nullptr) {
      *unsupported = context.unsupported.empty() ? "unsupported construct" : context.unsupported;
    }
    return std::nullopt;
  }
  return "SELECT " + *sql;
}

std::optional<QueryParameters> TranslateParameters(
    const std::variant<std::vector<googlesql::Value>, std::map<std::string, googlesql::Value>>&
        values,
    std::string* unsupported) {
  // Positional parameters are the ones without a name.
  std::vector<std::pair<std::string, googlesql::Value>> parameters;
  if (const auto* list = std::get_if<std::vector<googlesql::Value>>(&values)) {
    std::ranges::transform(*list, std::back_inserter(parameters),
                           [](const googlesql::Value& value) {
                             return std::pair<std::string, googlesql::Value>("", value);
                           });
  } else {
    const auto& map = std::get<std::map<std::string, googlesql::Value>>(values);
    parameters.assign(map.begin(), map.end());
  }
  std::vector<std::pair<std::string, std::string>> named;
  std::vector<std::string> positional;
  for (const auto& [name, value] : parameters) {
    std::optional<std::string> literal = translator::Literal(value);
    if (!literal) {
      *unsupported = "query parameters of type " + value.type()->DebugString();
      return std::nullopt;
    }
    if (name.empty()) {
      positional.push_back(*std::move(literal));
    } else {
      named.emplace_back(name, *std::move(literal));
    }
  }
  return QueryParameters(named, std::move(positional));
}

namespace {

// Whether values of `source` have the shape of `destination` where it rounds to even: NUMERIC or
// BIGNUMERIC values, in arrays where it has them, and STRUCT fields of the same names in order.
bool RoundsAs(const FieldSchema& source, const FieldSchema& destination) {
  if (!RoundsToEven(destination)) {
    return true;
  }
  if ((source.mode == FieldMode::kRepeated) != (destination.mode == FieldMode::kRepeated)) {
    return false;
  }
  if (destination.type != FieldType::kRecord) {
    return source.type == FieldType::kNumeric || source.type == FieldType::kBigNumeric;
  }
  if (source.type != FieldType::kRecord || source.fields.size() != destination.fields.size()) {
    return false;
  }
  for (std::size_t i = 0; i < source.fields.size(); ++i) {
    if (!absl::EqualsIgnoreCase(source.fields.at(i).name, destination.fields.at(i).name) ||
        !RoundsAs(source.fields.at(i), destination.fields.at(i))) {
      return false;
    }
  }
  return true;
}

}  // namespace

std::optional<std::string> TranslateAppendedValue(const function_catalog::Catalog& functions,
                                                  const std::string& value,
                                                  const FieldSchema& source,
                                                  const FieldSchema& destination) {
  if (!RoundsToEven(destination)) {
    return CheckedWriteValue(value, destination);
  }
  if (!RoundsAs(source, destination)) {
    return std::nullopt;
  }
  googlesql::TypeFactory type_factory;
  const absl::StatusOr<const googlesql::Type*> type = GoogleSqlType(source, &type_factory);
  if (!type.ok()) {
    return std::nullopt;
  }
  const auto rounded = translator::RoundedToEven(functions, value, *type, destination);
  return rounded ? std::optional<std::string>(CheckedWriteValue(*rounded, destination))
                 : std::nullopt;
}

}  // namespace bigquery_emulator_duckdb
