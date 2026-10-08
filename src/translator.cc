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
#include "googlesql/resolved_ast/resolved_ast.h"
#include "src/analyzer.h"
#include "src/duckdb_sql.h"
#include "src/query_parameters.h"
#include "src/translator/context.h"
#include "src/translator/ddl.h"
#include "src/translator/dml.h"
#include "src/translator/expression.h"
#include "src/translator/literal.h"
#include "src/translator/scan.h"

namespace bigquery_emulator_duckdb::translator {
namespace {

std::optional<std::string> Statement(const googlesql::ResolvedStatement& statement,
                                     const Scope& scope) {
  if (!statement.hint_list().empty()) {
    return Unsupported(scope, "statement hints");
  }
  if (statement.Is<googlesql::ResolvedInsertStmt>()) {
    return Insert(*statement.GetAs<googlesql::ResolvedInsertStmt>(), scope);
  }
  if (statement.Is<googlesql::ResolvedUpdateStmt>()) {
    return Update(*statement.GetAs<googlesql::ResolvedUpdateStmt>(), scope);
  }
  if (statement.Is<googlesql::ResolvedDeleteStmt>()) {
    return Delete(*statement.GetAs<googlesql::ResolvedDeleteStmt>(), scope);
  }
  if (statement.Is<googlesql::ResolvedTruncateStmt>()) {
    return Truncate(*statement.GetAs<googlesql::ResolvedTruncateStmt>(), scope);
  }
  if (statement.Is<googlesql::ResolvedMergeStmt>()) {
    return Merge(*statement.GetAs<googlesql::ResolvedMergeStmt>(), scope);
  }
  if (statement.Is<googlesql::ResolvedCreateTableStmt>()) {
    return CreateTable(*statement.GetAs<googlesql::ResolvedCreateTableStmt>(), scope);
  }
  if (statement.Is<googlesql::ResolvedAlterTableStmt>()) {
    return AlterTable(*statement.GetAs<googlesql::ResolvedAlterTableStmt>(), scope);
  }
  if (statement.Is<googlesql::ResolvedAlterTableSetOptionsStmt>()) {
    return AlterTableSetOptions(*statement.GetAs<googlesql::ResolvedAlterTableSetOptionsStmt>(),
                                scope);
  }
  if (statement.Is<googlesql::ResolvedCreateTableAsSelectStmt>()) {
    return CreateTableAsSelect(*statement.GetAs<googlesql::ResolvedCreateTableAsSelectStmt>(),
                               scope);
  }
  if (statement.Is<googlesql::ResolvedCreateViewStmt>()) {
    return CreateView(*statement.GetAs<googlesql::ResolvedCreateViewStmt>(), scope);
  }
  if (statement.Is<googlesql::ResolvedCreateSchemaStmt>()) {
    return CreateSchema(*statement.GetAs<googlesql::ResolvedCreateSchemaStmt>(), scope);
  }
  if (statement.Is<googlesql::ResolvedAlterSchemaStmt>()) {
    return AlterSchema(*statement.GetAs<googlesql::ResolvedAlterSchemaStmt>(), scope);
  }
  if (statement.Is<googlesql::ResolvedDropStmt>()) {
    return Drop(*statement.GetAs<googlesql::ResolvedDropStmt>(), scope);
  }
  if (!statement.Is<googlesql::ResolvedQueryStmt>()) {
    return Unsupported(scope, "statement " + statement.node_kind_string());
  }
  const auto* query = statement.GetAs<googlesql::ResolvedQueryStmt>();
  const auto relation = Scan(*query->query(), scope);
  if (!relation) {
    return std::nullopt;
  }
  std::vector<std::string> projections;
  // A value table of structs is returned as the structs' fields, like BigQuery does.
  if (query->is_value_table() && query->output_column_list_size() == 1 &&
      query->output_column_list(0)->column().type()->IsStruct()) {
    const auto& output = query->output_column_list(0)->column();
    const auto column = relation->columns.find(output.column_id());
    const auto& fields = output.type()->AsStruct()->fields();
    if (column == relation->columns.end() || fields.empty()) {
      return Unsupported(scope, "SELECT AS STRUCT without fields");
    }
    for (size_t i = 0; i < fields.size(); ++i) {
      projections.push_back(
          "struct_extract_at(" + column->second + ", " + std::to_string(i + 1) + ") AS " +
          QuoteIdentifier(ValueTableFieldName(fields.at(i).name, static_cast<int>(i))));
    }
    return "SELECT " + Join(projections, ", ") + relation->From() + relation->Order();
  }
  for (const auto& output : query->output_column_list()) {
    const auto column = relation->columns.find(output->column().column_id());
    if (column == relation->columns.end()) {
      return std::nullopt;
    }
    projections.push_back(column->second + " AS " + QuoteIdentifier(output->name()));
  }
  if (projections.empty()) {
    return std::nullopt;
  }
  return "SELECT " + Join(projections, ", ") + relation->From() + relation->Order();
}

// JobStatistics2.statementType of a statement the translator supports.
std::string StatementType(const googlesql::ResolvedStatement& statement) {
  if (statement.Is<googlesql::ResolvedInsertStmt>()) {
    return "INSERT";
  }
  if (statement.Is<googlesql::ResolvedUpdateStmt>()) {
    return "UPDATE";
  }
  if (statement.Is<googlesql::ResolvedDeleteStmt>()) {
    return "DELETE";
  }
  if (statement.Is<googlesql::ResolvedTruncateStmt>()) {
    return "TRUNCATE_TABLE";
  }
  if (statement.Is<googlesql::ResolvedMergeStmt>()) {
    return "MERGE";
  }
  if (statement.Is<googlesql::ResolvedCreateTableStmt>()) {
    return "CREATE_TABLE";
  }
  if (statement.Is<googlesql::ResolvedCreateTableAsSelectStmt>()) {
    return "CREATE_TABLE_AS_SELECT";
  }
  if (statement.Is<googlesql::ResolvedAlterTableStmt>() ||
      statement.Is<googlesql::ResolvedAlterTableSetOptionsStmt>()) {
    return "ALTER_TABLE";
  }
  if (statement.Is<googlesql::ResolvedAlterSchemaStmt>()) {
    return "ALTER_SCHEMA";
  }
  if (statement.Is<googlesql::ResolvedCreateViewStmt>()) {
    return "CREATE_VIEW";
  }
  if (statement.Is<googlesql::ResolvedCreateSchemaStmt>()) {
    return "CREATE_SCHEMA";
  }
  if (statement.Is<googlesql::ResolvedDropStmt>()) {
    return "DROP_" + ToUpperAscii(statement.GetAs<googlesql::ResolvedDropStmt>()->object_type());
  }
  return "SELECT";
}

}  // namespace
}  // namespace bigquery_emulator_duckdb::translator

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
      .table = std::move(context.table),
      .altered_table = std::move(context.altered_table),
      .view = std::move(context.view),
      .dataset = std::move(context.dataset),
      .altered_dataset = std::move(context.altered_dataset),
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
