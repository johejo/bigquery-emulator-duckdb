#include "src/translator.h"

#include <algorithm>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "googlesql/public/type.h"
#include "googlesql/public/value.h"
#include "src/analyzer.h"
#include "src/query_parameters.h"
#include "src/system_variables.h"
#include "src/translated_statement.h"
#include "src/translator/context.h"
#include "src/translator/expression.h"
#include "src/translator/literal.h"
#include "src/translator/statement.h"

namespace bigquery_emulator_duckdb {

std::optional<TranslatedStatement> TranslateStatement(
    const googlesql::ResolvedStatement& statement, const QueryParameters& parameters,
    const DefaultDataset& defaults, std::string* unsupported,
    const googlesql::SystemVariableValuesMap* system_variables) {
  translator::Context context{
      .parameters = parameters,
      .defaults = defaults,
      .system_variables = system_variables,
  };
  auto sql = translator::Statement(statement, translator::Scope{.context = context});
  if (!sql) {
    if (unsupported != nullptr) {
      *unsupported = context.unsupported.empty() ? "unsupported construct" : context.unsupported;
    }
    return std::nullopt;
  }
  return TranslatedStatement{
      .sql = *std::move(sql),
      .statement_type = translator::StatementType(statement),
      .result_schema = ResultSchema(statement),
      .ddl_target_table = std::move(context.ddl_target_table),
      .ddl_target_dataset = std::move(context.ddl_target_dataset),
      .ddl_target_routine = std::move(context.ddl_target_routine),
      .table = std::move(context.table),
      .altered_table = std::move(context.altered_table),
      .view = std::move(context.view),
      .dataset = std::move(context.dataset),
      .altered_dataset = std::move(context.altered_dataset),
      .routine = std::move(context.routine),
  };
}

std::optional<std::string> TranslateExpression(
    const googlesql::ResolvedExpr& expression, const QueryParameters& parameters,
    const DefaultDataset& defaults, std::string* unsupported,
    const googlesql::SystemVariableValuesMap* system_variables) {
  translator::Context context{
      .parameters = parameters,
      .defaults = defaults,
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

}  // namespace bigquery_emulator_duckdb
