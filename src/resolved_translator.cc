#include "src/resolved_translator.h"

#include <cmath>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "googlesql/public/catalog.h"
#include "googlesql/public/function.h"
#include "googlesql/public/strings.h"
#include "googlesql/public/type.h"
#include "googlesql/public/value.h"
#include "googlesql/resolved_ast/resolved_ast.h"
#include "src/duckdb_sql.h"
#include "src/functions.h"

namespace bigquery_emulator_duckdb {
namespace {

std::optional<std::string> SqlType(const googlesql::Type* type) {
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
    case googlesql::TYPE_DATE:
      return "DATE";
    case googlesql::TYPE_TIMESTAMP:
      return "TIMESTAMPTZ";
    case googlesql::TYPE_DATETIME:
      return "TIMESTAMP";
    case googlesql::TYPE_TIME:
      return "TIME";
    case googlesql::TYPE_NUMERIC:
      return "DECIMAL(38,9)";
    case googlesql::TYPE_JSON:
      return "JSON";
    case googlesql::TYPE_ARRAY: {
      const auto element = SqlType(type->AsArray()->element_type());
      return element ? std::optional<std::string>(*element + "[]") : std::nullopt;
    }
    default:
      return std::nullopt;
  }
}

// Each scan exposes synthetic names keyed by resolved column ID. User aliases only
// appear at the query boundary, so duplicate names and nested scopes cannot collide.
using Columns = std::map<int, std::string>;

std::string ColumnName(int id) { return QuoteIdentifier("_c" + std::to_string(id)); }

std::string Join(const std::vector<std::string>& parts, std::string_view separator) {
  std::string result;
  for (const auto& part : parts) {
    if (!result.empty()) {
      result += separator;
    }
    result += part;
  }
  return result;
}

std::optional<std::string> Literal(const googlesql::Value& value) {
  const auto type = SqlType(value.type());
  if (!type) {
    return std::nullopt;
  }
  std::string literal;
  if (value.is_null()) {
    literal = "NULL";
  } else if (value.type()->IsString()) {
    literal = QuoteLiteral(value.string_value());
  } else if (value.type()->IsBytes()) {
    literal = "from_hex(" + QuoteLiteral(ToHex(value.bytes_value())) + ")";
  } else if (value.type()->IsArray()) {
    std::vector<std::string> elements;
    for (int i = 0; i < value.num_elements(); ++i) {
      const auto sql = Literal(value.element(i));
      if (!sql) {
        return std::nullopt;
      }
      elements.push_back(*sql);
    }
    literal = "[" + Join(elements, ", ") + "]";
  } else if (value.type()->IsDouble() && !std::isfinite(value.double_value())) {
    literal = QuoteLiteral(std::isnan(value.double_value()) ? "nan"
                           : value.double_value() < 0       ? "-inf"
                                                            : "inf");
  } else {
    literal = value.GetSQLLiteral();
    if (value.type()->IsDate() || value.type()->IsTimestamp() || value.type()->IsDatetime() ||
        value.type()->IsTime() || value.type()->IsNumericType() || value.type()->IsJson()) {
      std::string contents;
      if (!googlesql::ParseStringLiteral(literal.substr(literal.find(' ') + 1), &contents).ok()) {
        return std::nullopt;
      }
      literal = QuoteLiteral(contents);
    }
  }
  return "CAST(" + literal + " AS " + *type + ")";
}

std::optional<std::string> Expression(const googlesql::ResolvedExpr& expr,
                                      const QueryParameters& parameters, const Columns& columns);

// Date parts are enum literals after analysis, not SQL identifier expressions.
std::optional<std::string> DatePart(const googlesql::ResolvedExpr& expr) {
  if (!expr.Is<googlesql::ResolvedLiteral>()) {
    return std::nullopt;
  }
  const auto& value = expr.GetAs<googlesql::ResolvedLiteral>()->value();
  if (value.is_null() || !value.type()->IsEnum()) {
    return std::nullopt;
  }
  const std::string part = ToLowerAscii(value.EnumDisplayName());
  static const std::set<std::string> supported = {
      "year",   "quarter", "month",       "week",        "day",     "hour",
      "minute", "second",  "millisecond", "microsecond", "isoyear", "isoweek"};
  return supported.contains(part) ? std::optional<std::string>(part) : std::nullopt;
}

std::string Expand(std::string_view spelling, const std::vector<std::string>& args) {
  std::string sql;
  for (size_t i = 0; i < spelling.size(); ++i) {
    if ((spelling[i] == '$' || spelling[i] == '#') && i + 1 < spelling.size() &&
        spelling[i + 1] >= '1' && spelling[i + 1] <= '9') {
      sql += args.at(static_cast<size_t>(spelling[++i] - '1'));
    } else {
      sql += spelling[i];
    }
  }
  return sql;
}

std::optional<std::string> Function(const googlesql::ResolvedFunctionCall& call,
                                    const QueryParameters& parameters, const Columns& columns) {
  if (!call.generic_argument_list().empty() || !call.hint_list().empty() ||
      !call.collation_list().empty() ||
      call.error_mode() != googlesql::ResolvedFunctionCallBase::DEFAULT_ERROR_MODE) {
    return std::nullopt;
  }
  std::string name = ToUpperAscii(call.function()->Name());
  // CONTAINS_SUBSTR is supplied by our catalog because GoogleSQL lacks this BigQuery builtin.
  if (!call.function()->IsGoogleSQLBuiltin() && name != "CONTAINS_SUBSTR") {
    return std::nullopt;
  }
  std::vector<std::string> args;
  for (const auto& argument : call.argument_list()) {
    auto sql = argument->type()->IsEnum() ? DatePart(*argument)
                                          : Expression(*argument, parameters, columns);
    if (!sql) {
      return std::nullopt;
    }
    args.push_back(argument->type()->IsEnum() ? QuoteLiteral(*sql) : *sql);
  }
  const size_t n = args.size();
  const auto invoke = [&](std::string_view function) {
    return std::string(function) + "(" + Join(args, ", ") + ")";
  };
  static const std::map<std::string, std::string> binary = {
      {"$ADD", "+"},        {"$SUBTRACT", "-"},
      {"$MULTIPLY", "*"},   {"$DIVIDE", "/"},
      {"$EQUAL", "="},      {"$NOT_EQUAL", "<>"},
      {"$LESS", "<"},       {"$LESS_OR_EQUAL", "<="},
      {"$GREATER", ">"},    {"$GREATER_OR_EQUAL", ">="},
      {"$LIKE", "LIKE"},    {"$BITWISE_AND", "&"},
      {"$BITWISE_OR", "|"}, {"$BITWISE_XOR", "^"}};
  if (const auto op = binary.find(name); op != binary.end() && n == 2) {
    if (name == "$BITWISE_XOR") {
      return invoke("xor");
    }
    if (name == "$DIVIDE") {
      // Bind both operands once, including volatile expressions, while keeping this an
      // expression that CASE/IF can short-circuit. DuckDB otherwise returns infinity on
      // zero. A scalar subquery here would break conditional evaluation of the error.
      return "list_transform([struct_pack(n := " + args[0] + ", d := " + args[1] +
             ")], _div -> CASE WHEN _div.n IS NULL OR _div.d IS NULL THEN NULL "
             "WHEN _div.d = 0 THEN error('division by zero') "
             "ELSE _div.n / _div.d END)[1]";
    }
    return "(" + args[0] + " " + op->second + " " + args[1] + ")";
  }
  if ((name == "$AND" || name == "$OR") && n >= 2) {
    return "(" + Join(args, name == "$AND" ? " AND " : " OR ") + ")";
  }
  if (n == 1) {
    if (name == "$NOT") {
      return "(NOT " + args[0] + ")";
    }
    if (name == "$UNARY_MINUS") {
      return "(-" + args[0] + ")";
    }
    if (name == "$BITWISE_NOT") {
      return "(~" + args[0] + ")";
    }
    if (name == "$IS_NULL") {
      return "(" + args[0] + " IS NULL)";
    }
    if (name == "$IS_TRUE") {
      return "(" + args[0] + " IS TRUE)";
    }
    if (name == "$IS_FALSE") {
      return "(" + args[0] + " IS FALSE)";
    }
  }
  if (name == "$BETWEEN" && n == 3) {
    return "(" + args[0] + " BETWEEN " + args[1] + " AND " + args[2] + ")";
  }
  if (name == "$IN" && n >= 2) {
    const std::vector<std::string> values(args.begin() + 1, args.end());
    return "(" + args[0] + " IN (" + Join(values, ", ") + "))";
  }
  if (name == "$CASE_NO_VALUE" || name == "$CASE_WITH_VALUE") {
    const size_t start = name == "$CASE_WITH_VALUE" ? 1 : 0;
    if (n < start + 3 || (n - start) % 2 != 1) {
      return std::nullopt;
    }
    std::string sql = "CASE ";
    if (start == 1) {
      sql += args[0] + " ";
    }
    for (size_t i = start; i + 1 < n; i += 2) {
      sql += "WHEN " + args[i] + " THEN " + args[i + 1] + " ";
    }
    return "(" + sql + "ELSE " + args.back() + " END)";
  }
  if (name == "BYTE_LENGTH" && n == 1) {
    if (call.argument_list(0)->type()->IsString()) {
      return invoke("strlen");
    }
    if (call.argument_list(0)->type()->IsBytes()) {
      return invoke("octet_length");
    }
    return std::nullopt;
  }
  if ((name == "CURRENT_TIMESTAMP" || name == "CURRENT_DATE" || name == "CURRENT_TIME") && n == 0) {
    return name;
  }
  if (name == "CURRENT_DATETIME" && n == 0) {
    return "CAST(CURRENT_TIMESTAMP AS TIMESTAMP)";
  }
  // INTERVAL n PART becomes two arguments (INT64, enum) in the resolved AST.
  if (n == 3 && (name == "DATE_ADD" || name == "DATE_SUB" || name == "DATETIME_ADD" ||
                 name == "DATETIME_SUB" || name == "TIMESTAMP_ADD" || name == "TIMESTAMP_SUB" ||
                 name == "TIME_ADD" || name == "TIME_SUB")) {
    const auto part = DatePart(*call.argument_list(2));
    if (!part || *part == "isoyear" || *part == "isoweek") {
      return std::nullopt;
    }
    const std::string interval = "(" + args[1] + " * INTERVAL '1 " + *part + "')";
    const auto spelling = DuckDbFunctionTemplate(name, 2);
    if (!spelling) {
      return std::nullopt;
    }
    return Expand(*spelling, {args[0], interval});
  }
  if (name == "PARSE_JSON" && n == 2) {
    const auto* mode = call.argument_list(1);
    if (!mode->Is<googlesql::ResolvedLiteral>()) {
      return std::nullopt;
    }
    const auto& value = mode->GetAs<googlesql::ResolvedLiteral>()->value();
    if (value.is_null() || !value.type()->IsString() || value.string_value() != "exact") {
      return std::nullopt;
    }
    return "json(" + args[0] + ")";
  }
  if (name == "LENGTH" && n == 1 && call.argument_list(0)->type()->IsBytes()) {
    return invoke("octet_length");
  }
  if (const auto spelling = DuckDbFunctionTemplate(name, n)) {
    return Expand(*spelling, args);
  }
  if (const auto renamed = DuckDbFunctionName(name)) {
    return invoke(*renamed);
  }
  // Passing arbitrary builtin names through would accidentally accept internal functions
  // and overloads with different semantics. Extend this list with execution coverage.
  static const std::set<std::string> plain = {"ABS",
                                              "SIGN",
                                              "ROUND",
                                              "TRUNC",
                                              "CEIL",
                                              "CEILING",
                                              "FLOOR",
                                              "SQRT",
                                              "POW",
                                              "POWER",
                                              "EXP",
                                              "LN",
                                              "LOG10",
                                              "MOD",
                                              "GREATEST",
                                              "LEAST",
                                              "IF",
                                              "IFNULL",
                                              "NULLIF",
                                              "COALESCE",
                                              "LENGTH",
                                              "CHAR_LENGTH",
                                              "CHARACTER_LENGTH",
                                              "LOWER",
                                              "UPPER",
                                              "CONCAT",
                                              "SUBSTR",
                                              "SUBSTRING",
                                              "TRIM",
                                              "LTRIM",
                                              "RTRIM",
                                              "REPLACE",
                                              "REVERSE",
                                              "REPEAT",
                                              "LPAD",
                                              "RPAD",
                                              "STARTS_WITH",
                                              "ENDS_WITH",
                                              "STRPOS",
                                              "SPLIT",
                                              "ARRAY_LENGTH",
                                              "ARRAY_TO_STRING"};
  return plain.contains(name) ? std::optional<std::string>(invoke(name)) : std::nullopt;
}

std::optional<std::string> Expression(const googlesql::ResolvedExpr& expr,
                                      const QueryParameters& parameters, const Columns& columns) {
  const auto type = SqlType(expr.type());
  if (!type || expr.type_annotation_map() != nullptr) {
    return std::nullopt;
  }
  if (expr.Is<googlesql::ResolvedLiteral>()) {
    return Literal(expr.GetAs<googlesql::ResolvedLiteral>()->value());
  }
  if (expr.Is<googlesql::ResolvedColumnRef>()) {
    const auto* ref = expr.GetAs<googlesql::ResolvedColumnRef>();
    const auto column = columns.find(ref->column().column_id());
    if (ref->is_correlated() || column == columns.end()) {
      return std::nullopt;
    }
    return column->second;
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
    const auto argument = Expression(*cast->expr(), parameters, columns);
    if (!argument) {
      return std::nullopt;
    }
    return std::string(cast->return_null_on_error() ? "TRY_CAST(" : "CAST(") + *argument + " AS " +
           *type + ")";
  }
  if (expr.Is<googlesql::ResolvedFunctionCall>()) {
    const auto sql = Function(*expr.GetAs<googlesql::ResolvedFunctionCall>(), parameters, columns);
    // Pin the resolved result type (uuid(), date arithmetic and integer functions can differ).
    return sql ? std::optional<std::string>("CAST(" + *sql + " AS " + *type + ")") : std::nullopt;
  }
  return std::nullopt;
}

struct Relation {
  std::string sql;
  Columns columns;
  // Sort keys may be absent from the visible projection. Carry them until the query boundary
  // and emit ORDER BY there too, rather than relying on order surviving a subquery.
  std::vector<std::string> ordering;

  std::string From() const { return " FROM (" + sql + ") AS q"; }
  std::string Order() const { return ordering.empty() ? "" : " ORDER BY " + Join(ordering, ", "); }
};

std::optional<Relation> Scan(const googlesql::ResolvedScan& scan,
                             const QueryParameters& parameters) {
  if (!scan.hint_list().empty()) {
    return std::nullopt;
  }
  if (scan.Is<googlesql::ResolvedSingleRowScan>()) {
    return Relation{"SELECT 1 AS _unit", {}, {}};
  }
  if (scan.Is<googlesql::ResolvedTableScan>()) {
    const auto* table = scan.GetAs<googlesql::ResolvedTableScan>();
    if (table->for_system_time_expr() != nullptr || table->lock_mode() != nullptr ||
        table->read_as_row_type() || table->table()->IsValueTable() ||
        table->column_list_size() != table->column_index_list_size()) {
      return std::nullopt;
    }
    Relation result;
    std::vector<std::string> projections;
    for (int i = 0; i < table->column_list_size(); ++i) {
      const auto& column = table->column_list(i);
      if (!SqlType(column.type()) || column.type_annotation_map() != nullptr) {
        return std::nullopt;
      }
      const std::string alias = ColumnName(column.column_id());
      projections.push_back(
          QuoteIdentifier(table->table()->GetColumn(table->column_index_list(i))->Name()) + " AS " +
          alias);
      result.columns.emplace(column.column_id(), "q." + alias);
    }
    result.sql = "SELECT " + (projections.empty() ? "1 AS _unit" : Join(projections, ", ")) +
                 " FROM " + QuoteIdentifierPath(table->table()->FullName());
    return result;
  }
  const googlesql::ResolvedScan* input = nullptr;
  if (scan.Is<googlesql::ResolvedProjectScan>()) {
    input = scan.GetAs<googlesql::ResolvedProjectScan>()->input_scan();
  }
  if (scan.Is<googlesql::ResolvedFilterScan>()) {
    input = scan.GetAs<googlesql::ResolvedFilterScan>()->input_scan();
  }
  if (scan.Is<googlesql::ResolvedOrderByScan>()) {
    input = scan.GetAs<googlesql::ResolvedOrderByScan>()->input_scan();
  }
  if (scan.Is<googlesql::ResolvedLimitOffsetScan>()) {
    input = scan.GetAs<googlesql::ResolvedLimitOffsetScan>()->input_scan();
  }
  if (input == nullptr) {
    return std::nullopt;
  }
  auto result = Scan(*input, parameters);
  if (!result) {
    return std::nullopt;
  }
  const std::string from = result->From();
  if (scan.Is<googlesql::ResolvedProjectScan>()) {
    const auto* project = scan.GetAs<googlesql::ResolvedProjectScan>();
    const Columns input_columns = result->columns;
    std::vector<std::string> projections;
    for (const auto& [id, sql] : input_columns) {
      projections.push_back(sql + " AS " + ColumnName(id));
    }
    for (const auto& computed : project->expr_list()) {
      const auto expression = Expression(*computed->expr(), parameters, input_columns);
      if (!expression) {
        return std::nullopt;
      }
      const int id = computed->column().column_id();
      projections.push_back(*expression + " AS " + ColumnName(id));
      result->columns.emplace(id, "q." + ColumnName(id));
    }
    result->sql = "SELECT " + (projections.empty() ? "1 AS _unit" : Join(projections, ", ")) + from;
  } else if (scan.Is<googlesql::ResolvedFilterScan>()) {
    const auto filter = Expression(*scan.GetAs<googlesql::ResolvedFilterScan>()->filter_expr(),
                                   parameters, result->columns);
    if (!filter) {
      return std::nullopt;
    }
    result->sql = "SELECT q.*" + from + " WHERE " + *filter;
  } else if (scan.Is<googlesql::ResolvedOrderByScan>()) {
    result->ordering.clear();
    for (const auto& item : scan.GetAs<googlesql::ResolvedOrderByScan>()->order_by_item_list()) {
      if (item->collation_name() != nullptr || !item->collation().Empty()) {
        return std::nullopt;
      }
      const auto column = Expression(*item->column_ref(), parameters, result->columns);
      if (!column) {
        return std::nullopt;
      }
      // BigQuery sorts NULL first for ASC and last for DESC, unlike DuckDB's ASC default.
      const bool nulls_first =
          item->null_order() == googlesql::ResolvedOrderByItem::NULLS_FIRST ||
          (item->null_order() == googlesql::ResolvedOrderByItem::ORDER_UNSPECIFIED &&
           !item->is_descending());
      result->ordering.push_back(*column + (item->is_descending() ? " DESC" : " ASC") +
                                 (nulls_first ? " NULLS FIRST" : " NULLS LAST"));
    }
  } else {
    const auto* limit = scan.GetAs<googlesql::ResolvedLimitOffsetScan>();
    const auto count = Expression(*limit->limit(), parameters, {});
    if (!count) {
      return std::nullopt;
    }
    result->sql = "SELECT q.*" + from + result->Order() + " LIMIT " + *count;
    if (limit->offset() != nullptr) {
      const auto offset = Expression(*limit->offset(), parameters, {});
      if (!offset) {
        return std::nullopt;
      }
      result->sql += " OFFSET " + *offset;
    }
  }
  // Ordering from a subquery is not a promise of ordering for its parent scan.
  if (!scan.is_ordered()) {
    result->ordering.clear();
  }
  return result;
}

}  // namespace

std::optional<std::string> TranslateResolvedToDuckDbSql(
    const googlesql::ResolvedStatement& statement, const QueryParameters& parameters) {
  if (!statement.Is<googlesql::ResolvedQueryStmt>() || !statement.hint_list().empty()) {
    return std::nullopt;
  }
  const auto* query = statement.GetAs<googlesql::ResolvedQueryStmt>();
  if (query->is_value_table()) {
    return std::nullopt;
  }
  const auto relation = Scan(*query->query(), parameters);
  if (!relation) {
    return std::nullopt;
  }
  std::vector<std::string> projections;
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

}  // namespace bigquery_emulator_duckdb
