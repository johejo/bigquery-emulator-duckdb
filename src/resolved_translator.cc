#include "src/resolved_translator.h"

#include <cmath>
#include <map>
#include <optional>
#include <string>
#include <utility>

#include "googlesql/public/function.h"
#include "googlesql/public/type.h"
#include "googlesql/public/value.h"
#include "googlesql/resolved_ast/resolved_ast.h"
#include "src/duckdb_sql.h"

namespace bigquery_emulator_duckdb {
namespace {

std::optional<std::string> ScalarType(const googlesql::Type* type) {
  switch (type->kind()) {
    case googlesql::TYPE_INT64:
      return "BIGINT";
    case googlesql::TYPE_DOUBLE:
      return "DOUBLE";
    case googlesql::TYPE_BOOL:
      return "BOOLEAN";
    case googlesql::TYPE_STRING:
      return "VARCHAR";
    case googlesql::TYPE_BYTES:
      return "BLOB";
    default:
      return std::nullopt;
  }
}

std::optional<std::string> Expression(const googlesql::ResolvedExpr& expr,
                                      const QueryParameters& parameters) {
  const auto type = ScalarType(expr.type());
  if (!type.has_value() || expr.type_annotation_map() != nullptr) {
    return std::nullopt;
  }
  if (expr.Is<googlesql::ResolvedLiteral>()) {
    const auto& value = expr.GetAs<googlesql::ResolvedLiteral>()->value();
    std::string literal;
    if (value.is_null()) {
      literal = "NULL";
    } else if (value.type()->IsString()) {
      literal = QuoteLiteral(value.string_value());
    } else if (value.type()->IsBytes()) {
      literal = "from_hex(" + QuoteLiteral(ToHex(value.bytes_value())) + ")";
    } else if (value.type()->IsDouble() && !std::isfinite(value.double_value())) {
      return std::nullopt;
    } else {
      literal = value.GetSQLLiteral();
    }
    return "CAST(" + literal + " AS " + *type + ")";
  }
  if (expr.Is<googlesql::ResolvedParameter>()) {
    const auto* parameter = expr.GetAs<googlesql::ResolvedParameter>();
    return parameter->name().empty() ? parameters.ByPosition(parameter->position())
                                     : parameters.ByName(parameter->name());
  }
  if (expr.Is<googlesql::ResolvedCast>()) {
    const auto* cast = expr.GetAs<googlesql::ResolvedCast>();
    if (cast->format() != nullptr || cast->time_zone() != nullptr ||
        cast->extended_cast() != nullptr || !cast->type_modifiers().IsEmpty()) {
      return std::nullopt;
    }
    const auto argument = Expression(*cast->expr(), parameters);
    if (!argument.has_value()) {
      return std::nullopt;
    }
    return std::string(cast->return_null_on_error() ? "TRY_CAST(" : "CAST(") + *argument + " AS " +
           *type + ")";
  }
  if (expr.Is<googlesql::ResolvedFunctionCall>()) {
    const auto* call = expr.GetAs<googlesql::ResolvedFunctionCall>();
    // Resolved calls include internal functions and overloads. Only lower signatures
    // whose semantics we implement, rather than passing unknown function names through.
    if (!call->function()->IsGoogleSQLBuiltin() ||
        ToUpperAscii(call->function()->Name()) != "BYTE_LENGTH" ||
        call->argument_list_size() != 1 || !call->generic_argument_list().empty() ||
        !call->hint_list().empty() || !call->collation_list().empty() ||
        call->error_mode() != googlesql::ResolvedFunctionCallBase::DEFAULT_ERROR_MODE) {
      return std::nullopt;
    }
    const auto* argument = call->argument_list(0);
    if (!argument->type()->IsString() && !argument->type()->IsBytes()) {
      return std::nullopt;
    }
    const auto sql = Expression(*argument, parameters);
    if (!sql.has_value()) {
      return std::nullopt;
    }
    return std::string(argument->type()->IsString() ? "strlen(" : "octet_length(") + *sql + ")";
  }
  return std::nullopt;
}

}  // namespace

std::optional<std::string> TranslateResolvedToDuckDbSql(
    const googlesql::ResolvedStatement& statement, const QueryParameters& parameters) {
  if (!statement.Is<googlesql::ResolvedQueryStmt>() || !statement.hint_list().empty()) {
    return std::nullopt;
  }
  const auto* query = statement.GetAs<googlesql::ResolvedQueryStmt>();
  if (query->is_value_table() || !query->query()->Is<googlesql::ResolvedProjectScan>()) {
    return std::nullopt;
  }
  const auto* project = query->query()->GetAs<googlesql::ResolvedProjectScan>();
  if (!project->input_scan()->Is<googlesql::ResolvedSingleRowScan>() ||
      !project->hint_list().empty() || !project->input_scan()->hint_list().empty()) {
    return std::nullopt;
  }
  std::map<int, std::string> columns;
  for (const auto& computed : project->expr_list()) {
    auto expression = Expression(*computed->expr(), parameters);
    if (!expression.has_value()) {
      return std::nullopt;
    }
    columns.emplace(computed->column().column_id(), *std::move(expression));
  }
  std::string sql = "SELECT ";
  bool first = true;
  for (const auto& output : query->output_column_list()) {
    const auto column = columns.find(output->column().column_id());
    if (column == columns.end()) {
      return std::nullopt;
    }
    if (!first) {
      sql += ", ";
    }
    first = false;
    sql += column->second + " AS " + QuoteIdentifier(output->name());
  }
  return first ? std::nullopt : std::optional<std::string>(std::move(sql));
}

}  // namespace bigquery_emulator_duckdb
