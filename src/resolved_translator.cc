#include "src/resolved_translator.h"

#include <cmath>
#include <map>
#include <memory>
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

struct WithQuery {
  std::string name;
  size_t width;
};

// What a scan can see besides its own input: correlated columns of enclosing queries, which
// are referenced unqualified because every scope aliases its input as q, and named WITH queries.
struct Scope {
  const QueryParameters& parameters;
  int& next_name;
  Columns outer;
  std::map<std::string, WithQuery> with;
};

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
    case googlesql::TYPE_STRUCT: {
      // DuckDB structs need distinct field names, which anonymous BigQuery fields lack.
      std::set<std::string> names;
      std::vector<std::string> fields;
      for (const auto& field : type->AsStruct()->fields()) {
        const auto field_type = SqlType(field.type);
        if (!field_type || field.name.empty() || !names.insert(ToLowerAscii(field.name)).second) {
          return std::nullopt;
        }
        fields.push_back(QuoteIdentifier(field.name) + " " + *field_type);
      }
      return fields.empty() ? std::nullopt
                            : std::optional<std::string>("STRUCT(" + Join(fields, ", ") + ")");
    }
    default:
      return std::nullopt;
  }
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
  } else if (value.type()->IsStruct()) {
    std::vector<std::string> fields;
    for (int i = 0; i < value.num_fields(); ++i) {
      const auto sql = Literal(value.field(i));
      if (!sql) {
        return std::nullopt;
      }
      fields.push_back(QuoteIdentifier(value.type()->AsStruct()->field(i).name) + " := " + *sql);
    }
    literal = "struct_pack(" + Join(fields, ", ") + ")";
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

struct Relation {
  std::string sql;
  Columns columns;
  // Sort keys may be absent from the visible projection. Carry them until the query boundary
  // and emit ORDER BY there too, rather than relying on order surviving a subquery.
  std::vector<std::string> ordering;

  std::string From() const { return " FROM (" + sql + ") AS q"; }
  std::string Order() const { return ordering.empty() ? "" : " ORDER BY " + Join(ordering, ", "); }
};

std::optional<Relation> Scan(const googlesql::ResolvedScan& scan, const Scope& scope);

std::optional<std::string> Expression(const googlesql::ResolvedExpr& expr, const Scope& scope,
                                      const Columns& columns);

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

std::optional<std::string> Function(const googlesql::ResolvedFunctionCall& call, const Scope& scope,
                                    const Columns& columns) {
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
    auto sql =
        argument->type()->IsEnum() ? DatePart(*argument) : Expression(*argument, scope, columns);
    if (!sql) {
      return std::nullopt;
    }
    args.push_back(argument->type()->IsEnum() ? QuoteLiteral(*sql) : *sql);
  }
  const size_t n = args.size();
  const auto invoke = [&](std::string_view function) {
    return std::string(function) + "(" + Join(args, ", ") + ")";
  };
  if (name == "$MAKE_ARRAY") {
    return "[" + Join(args, ", ") + "]";
  }
  if (n == 2 && (name == "$ARRAY_AT_OFFSET" || name == "$ARRAY_AT_ORDINAL" ||
                 name == "$SAFE_ARRAY_AT_OFFSET" || name == "$SAFE_ARRAY_AT_ORDINAL")) {
    // Bound like division so both operands are evaluated once. DuckDB would return NULL for
    // an index out of range, and count negative indexes from the end.
    const bool ordinal = name.ends_with("ORDINAL");
    const std::string out_of_range =
        ordinal ? "_at.i < 1 OR _at.i > len(_at.a)" : "_at.i < 0 OR _at.i >= len(_at.a)";
    return "list_transform([struct_pack(a := " + args[0] + ", i := " + args[1] +
           ")], _at -> CASE WHEN _at.a IS NULL OR _at.i IS NULL THEN NULL WHEN " + out_of_range +
           " THEN " +
           (name.starts_with("$SAFE_") ? "NULL"
                                       : "error('Array index ' || _at.i || ' is out of bounds')") +
           " ELSE " + (ordinal ? "_at.a[_at.i]" : "_at.a[_at.i + 1]") + " END)[1]";
  }
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

// Correlated references in a subquery name the enclosing columns without the q qualifier, which
// the subquery's own scopes shadow. Column IDs are unique per statement, so nothing collides.
Scope Nested(const Scope& scope, const Columns& columns) {
  Scope nested = scope;
  for (const auto& [id, sql] : columns) {
    const std::string name = ColumnName(id);
    if (sql.ends_with("." + name)) {
      nested.outer[id] = name;
    }
  }
  return nested;
}

std::optional<std::string> Subquery(const googlesql::ResolvedSubqueryExpr& subquery,
                                    const std::string& type, const Scope& scope,
                                    const Columns& columns) {
  if (!subquery.hint_list().empty()) {
    return std::nullopt;
  }
  const auto relation = Scan(*subquery.subquery(), Nested(scope, columns));
  if (!relation) {
    return std::nullopt;
  }
  if (subquery.subquery_type() == googlesql::ResolvedSubqueryExpr::EXISTS) {
    return "EXISTS (SELECT 1" + relation->From() + ")";
  }
  if (subquery.subquery()->column_list_size() != 1) {
    return std::nullopt;
  }
  const auto column = relation->columns.find(subquery.subquery()->column_list(0).column_id());
  if (column == relation->columns.end()) {
    return std::nullopt;
  }
  const std::string select = "SELECT " + column->second + relation->From();
  switch (subquery.subquery_type()) {
    case googlesql::ResolvedSubqueryExpr::SCALAR:
      return "CAST((" + select + ") AS " + type + ")";
    case googlesql::ResolvedSubqueryExpr::ARRAY:
      return "CAST(ARRAY(" + select + relation->Order() + ") AS " + type + ")";
    case googlesql::ResolvedSubqueryExpr::IN: {
      if (!subquery.in_collation().Empty()) {
        return std::nullopt;
      }
      const auto value = Expression(*subquery.in_expr(), scope, columns);
      if (!value) {
        return std::nullopt;
      }
      return "(" + *value + " IN (" + select + "))";
    }
    default:
      return std::nullopt;
  }
}

std::optional<std::string> Expression(const googlesql::ResolvedExpr& expr, const Scope& scope,
                                      const Columns& columns) {
  const auto type = SqlType(expr.type());
  if (!type || expr.type_annotation_map() != nullptr) {
    return std::nullopt;
  }
  if (expr.Is<googlesql::ResolvedLiteral>()) {
    return Literal(expr.GetAs<googlesql::ResolvedLiteral>()->value());
  }
  if (expr.Is<googlesql::ResolvedColumnRef>()) {
    const auto* ref = expr.GetAs<googlesql::ResolvedColumnRef>();
    const Columns& visible = ref->is_correlated() ? scope.outer : columns;
    const auto column = visible.find(ref->column().column_id());
    if (column == visible.end()) {
      return std::nullopt;
    }
    return column->second;
  }
  if (expr.Is<googlesql::ResolvedParameter>()) {
    const auto* parameter = expr.GetAs<googlesql::ResolvedParameter>();
    return parameter->name().empty() ? scope.parameters.ByPosition(parameter->position())
                                     : scope.parameters.ByName(parameter->name());
  }
  if (expr.Is<googlesql::ResolvedCast>()) {
    const auto* cast = expr.GetAs<googlesql::ResolvedCast>();
    if (cast->format() != nullptr || cast->time_zone() != nullptr ||
        cast->extended_cast() != nullptr || !cast->type_modifiers().IsEmpty()) {
      return std::nullopt;
    }
    const auto argument = Expression(*cast->expr(), scope, columns);
    if (!argument) {
      return std::nullopt;
    }
    return std::string(cast->return_null_on_error() ? "TRY_CAST(" : "CAST(") + *argument + " AS " +
           *type + ")";
  }
  if (expr.Is<googlesql::ResolvedFunctionCall>()) {
    const auto sql = Function(*expr.GetAs<googlesql::ResolvedFunctionCall>(), scope, columns);
    // Pin the resolved result type (uuid(), date arithmetic and integer functions can differ).
    return sql ? std::optional<std::string>("CAST(" + *sql + " AS " + *type + ")") : std::nullopt;
  }
  if (expr.Is<googlesql::ResolvedMakeStruct>()) {
    const auto* make = expr.GetAs<googlesql::ResolvedMakeStruct>();
    std::vector<std::string> fields;
    for (int i = 0; i < make->field_list_size(); ++i) {
      const auto field = Expression(*make->field_list(i), scope, columns);
      if (!field) {
        return std::nullopt;
      }
      fields.push_back(QuoteIdentifier(expr.type()->AsStruct()->field(i).name) + " := " + *field);
    }
    return "CAST(struct_pack(" + Join(fields, ", ") + ") AS " + *type + ")";
  }
  if (expr.Is<googlesql::ResolvedGetStructField>()) {
    const auto* get = expr.GetAs<googlesql::ResolvedGetStructField>();
    const auto input = Expression(*get->expr(), scope, columns);
    if (!input) {
      return std::nullopt;
    }
    return "struct_extract_at(" + *input + ", " + std::to_string(get->field_idx() + 1) + ")";
  }
  if (expr.Is<googlesql::ResolvedSubqueryExpr>()) {
    return Subquery(*expr.GetAs<googlesql::ResolvedSubqueryExpr>(), *type, scope, columns);
  }
  return std::nullopt;
}

std::optional<std::string> OrderItem(const googlesql::ResolvedOrderByItem& item, const Scope& scope,
                                     const Columns& columns) {
  if (item.collation_name() != nullptr || !item.collation().Empty()) {
    return std::nullopt;
  }
  const auto column = Expression(*item.column_ref(), scope, columns);
  if (!column) {
    return std::nullopt;
  }
  // BigQuery sorts NULL first for ASC and last for DESC, unlike DuckDB's ASC default.
  const bool nulls_first =
      item.null_order() == googlesql::ResolvedOrderByItem::NULLS_FIRST ||
      (item.null_order() == googlesql::ResolvedOrderByItem::ORDER_UNSPECIFIED &&
       !item.is_descending());
  return *column + (item.is_descending() ? " DESC" : " ASC") +
         (nulls_first ? " NULLS FIRST" : " NULLS LAST");
}

std::optional<std::string> OrderItems(
    const std::vector<std::unique_ptr<const googlesql::ResolvedOrderByItem>>& items,
    const Scope& scope, const Columns& columns) {
  std::vector<std::string> sql;
  for (const auto& item : items) {
    const auto order = OrderItem(*item, scope, columns);
    if (!order) {
      return std::nullopt;
    }
    sql.push_back(*order);
  }
  return Join(sql, ", ");
}

// Aggregate calls (with `aggregate` set) and analytic calls (with a non-empty `over`).
std::optional<std::string> NonScalarCall(const googlesql::ResolvedNonScalarFunctionCallBase& call,
                                         const googlesql::ResolvedAggregateFunctionCall* aggregate,
                                         const std::string& over, const Scope& scope,
                                         const Columns& columns) {
  if (!call.generic_argument_list().empty() || !call.hint_list().empty() ||
      !call.collation_list().empty() ||
      call.error_mode() != googlesql::ResolvedFunctionCallBase::DEFAULT_ERROR_MODE ||
      !call.function()->IsGoogleSQLBuiltin() || call.where_expr() != nullptr) {
    return std::nullopt;
  }
  if (aggregate != nullptr &&
      (aggregate->having_modifier() != nullptr || aggregate->limit() != nullptr ||
       !aggregate->group_by_list().empty() || !aggregate->group_by_aggregate_list().empty() ||
       aggregate->having_expr() != nullptr)) {
    return std::nullopt;
  }
  static const std::map<std::string, std::string> aggregates = {{"COUNT", "count"},
                                                                {"$COUNT_STAR", "count"},
                                                                {"SUM", "sum"},
                                                                {"AVG", "avg"},
                                                                {"MIN", "min"},
                                                                {"MAX", "max"},
                                                                {"ANY_VALUE", "any_value"},
                                                                {"ARRAY_AGG", "list"},
                                                                {"STRING_AGG", "string_agg"},
                                                                {"COUNTIF", "count_if"},
                                                                {"LOGICAL_AND", "bool_and"},
                                                                {"LOGICAL_OR", "bool_or"},
                                                                {"BIT_AND", "bit_and"},
                                                                {"BIT_OR", "bit_or"},
                                                                {"BIT_XOR", "bit_xor"},
                                                                {"STDDEV", "stddev_samp"},
                                                                {"STDDEV_SAMP", "stddev_samp"},
                                                                {"STDDEV_POP", "stddev_pop"},
                                                                {"VARIANCE", "var_samp"},
                                                                {"VAR_SAMP", "var_samp"},
                                                                {"VAR_POP", "var_pop"},
                                                                {"CORR", "corr"},
                                                                {"COVAR_POP", "covar_pop"},
                                                                {"COVAR_SAMP", "covar_samp"}};
  static const std::map<std::string, std::string> analytics = {{"ROW_NUMBER", "row_number"},
                                                               {"RANK", "rank"},
                                                               {"DENSE_RANK", "dense_rank"},
                                                               {"PERCENT_RANK", "percent_rank"},
                                                               {"CUME_DIST", "cume_dist"},
                                                               {"NTILE", "ntile"},
                                                               {"LAG", "lag"},
                                                               {"LEAD", "lead"},
                                                               {"FIRST_VALUE", "first_value"},
                                                               {"LAST_VALUE", "last_value"},
                                                               {"NTH_VALUE", "nth_value"}};
  const std::string name = ToUpperAscii(call.function()->Name());
  auto function = aggregates.find(name);
  if (function == aggregates.end()) {
    function = analytics.find(name);
    if (function == analytics.end() || over.empty()) {
      return std::nullopt;
    }
  }
  // DuckDB has no DISTINCT window aggregates.
  if (call.distinct() && !over.empty()) {
    return std::nullopt;
  }
  if (name == "STRING_AGG" && !call.argument_list(0)->type()->IsString()) {
    return std::nullopt;
  }
  std::vector<std::string> args;
  for (const auto& argument : call.argument_list()) {
    const auto sql = Expression(*argument, scope, columns);
    if (!sql) {
      return std::nullopt;
    }
    args.push_back(*sql);
  }
  std::string inner = name == "$COUNT_STAR" ? "*" : Join(args, ", ");
  if (call.distinct()) {
    inner = "DISTINCT " + inner;
  }
  std::string filter;
  if (call.null_handling_modifier() == googlesql::ResolvedNonScalarFunctionCallBase::IGNORE_NULLS) {
    if (name == "ARRAY_AGG") {
      filter = " FILTER (WHERE " + args.at(0) + " IS NOT NULL)";
    } else if (name == "FIRST_VALUE" || name == "LAST_VALUE" || name == "NTH_VALUE") {
      inner += " IGNORE NULLS";
    } else {
      return std::nullopt;
    }
  }
  if (aggregate != nullptr && !aggregate->order_by_item_list().empty()) {
    const auto order = OrderItems(aggregate->order_by_item_list(), scope, columns);
    if (!order) {
      return std::nullopt;
    }
    inner += " ORDER BY " + *order;
  }
  return function->second + "(" + inner + ")" + filter + over;
}

std::optional<std::string> FrameBound(const googlesql::ResolvedWindowFrameExpr& bound,
                                      const Scope& scope) {
  switch (bound.boundary_type()) {
    case googlesql::ResolvedWindowFrameExpr::UNBOUNDED_PRECEDING:
      return "UNBOUNDED PRECEDING";
    case googlesql::ResolvedWindowFrameExpr::CURRENT_ROW:
      return "CURRENT ROW";
    case googlesql::ResolvedWindowFrameExpr::UNBOUNDED_FOLLOWING:
      return "UNBOUNDED FOLLOWING";
    case googlesql::ResolvedWindowFrameExpr::OFFSET_PRECEDING:
    case googlesql::ResolvedWindowFrameExpr::OFFSET_FOLLOWING: {
      const auto offset = Expression(*bound.expression(), scope, {});
      if (!offset) {
        return std::nullopt;
      }
      return *offset +
             (bound.boundary_type() == googlesql::ResolvedWindowFrameExpr::OFFSET_PRECEDING
                  ? " PRECEDING"
                  : " FOLLOWING");
    }
    default:
      return std::nullopt;
  }
}

std::optional<std::string> Window(const googlesql::ResolvedAnalyticFunctionGroup& group,
                                  const googlesql::ResolvedWindowFrame* frame, const Scope& scope,
                                  const Columns& columns) {
  std::vector<std::string> window;
  if (const auto* partition = group.partition_by(); partition != nullptr) {
    if (!partition->hint_list().empty() || !partition->collation_list().empty()) {
      return std::nullopt;
    }
    std::vector<std::string> keys;
    for (const auto& ref : partition->partition_by_list()) {
      const auto key = Expression(*ref, scope, columns);
      if (!key) {
        return std::nullopt;
      }
      keys.push_back(*key);
    }
    window.push_back("PARTITION BY " + Join(keys, ", "));
  }
  if (const auto* ordering = group.order_by(); ordering != nullptr) {
    if (!ordering->hint_list().empty()) {
      return std::nullopt;
    }
    const auto order = OrderItems(ordering->order_by_item_list(), scope, columns);
    if (!order) {
      return std::nullopt;
    }
    window.push_back("ORDER BY " + *order);
  }
  if (frame != nullptr) {
    const auto start = FrameBound(*frame->start_expr(), scope);
    const auto end = FrameBound(*frame->end_expr(), scope);
    if (!start || !end) {
      return std::nullopt;
    }
    window.push_back(std::string(frame->frame_unit() == googlesql::ResolvedWindowFrame::ROWS
                                     ? "ROWS"
                                     : "RANGE") +
                     " BETWEEN " + *start + " AND " + *end);
  }
  return " OVER (" + Join(window, " ") + ")";
}

// Projects `columns` of a relation, positionally renamed to `names`.
std::optional<std::vector<std::string>> Renamed(
    const Relation& relation, const std::vector<googlesql::ResolvedColumn>& columns,
    const std::vector<std::string>& names) {
  std::vector<std::string> projections;
  for (size_t i = 0; i < columns.size(); ++i) {
    const auto column = relation.columns.find(columns[i].column_id());
    if (column == relation.columns.end()) {
      return std::nullopt;
    }
    projections.push_back(column->second + " AS " + names.at(i));
  }
  return projections;
}

std::optional<Relation> JoinScan(const googlesql::ResolvedJoinScan& join, const Scope& scope) {
  if (join.is_lateral() || !join.parameter_list().empty()) {
    return std::nullopt;
  }
  const auto left = Scan(*join.left_scan(), scope);
  const auto right = Scan(*join.right_scan(), scope);
  if (!left || !right) {
    return std::nullopt;
  }
  Relation result;
  Columns visible;
  std::vector<std::string> projections;
  for (const auto& [side, relation] : {std::pair{"l.", &*left}, std::pair{"r.", &*right}}) {
    for (const auto& [id, sql] : relation->columns) {
      visible.emplace(id, side + ColumnName(id));
      projections.push_back(side + ColumnName(id) + " AS " + ColumnName(id));
      result.columns.emplace(id, "q." + ColumnName(id));
    }
  }
  std::string kind;
  switch (join.join_type()) {
    case googlesql::ResolvedJoinScan::INNER:
      kind = "INNER";
      break;
    case googlesql::ResolvedJoinScan::LEFT:
      kind = "LEFT";
      break;
    case googlesql::ResolvedJoinScan::RIGHT:
      kind = "RIGHT";
      break;
    case googlesql::ResolvedJoinScan::FULL:
      kind = "FULL";
      break;
    default:
      return std::nullopt;
  }
  std::string condition = " ON TRUE";
  if (join.join_expr() != nullptr) {
    const auto on = Expression(*join.join_expr(), scope, visible);
    if (!on) {
      return std::nullopt;
    }
    condition = " ON " + *on;
  } else if (kind == "INNER") {
    kind = "CROSS";
    condition.clear();
  }
  result.sql = "SELECT " + (projections.empty() ? "1 AS _unit" : Join(projections, ", ")) +
               " FROM (" + left->sql + ") AS l " + kind + " JOIN (" + right->sql + ") AS r" +
               condition;
  return result;
}

std::optional<Relation> AggregateScan(const googlesql::ResolvedAggregateScan& aggregate,
                                      const Scope& scope) {
  if (!aggregate.grouping_set_list().empty() || !aggregate.rollup_column_list().empty() ||
      !aggregate.grouping_call_list().empty() || !aggregate.collation_list().empty()) {
    return std::nullopt;
  }
  const auto input = Scan(*aggregate.input_scan(), scope);
  if (!input) {
    return std::nullopt;
  }
  Relation result;
  std::vector<std::string> projections;
  std::vector<std::string> keys;
  for (const auto& key : aggregate.group_by_list()) {
    const auto sql = Expression(*key->expr(), scope, input->columns);
    if (!sql) {
      return std::nullopt;
    }
    const int id = key->column().column_id();
    projections.push_back(*sql + " AS " + ColumnName(id));
    keys.push_back(std::to_string(projections.size()));
    result.columns.emplace(id, "q." + ColumnName(id));
  }
  for (const auto& computed : aggregate.aggregate_list()) {
    const auto* call = computed->expr()->GetAs<googlesql::ResolvedAggregateFunctionCall>();
    const auto type = SqlType(computed->expr()->type());
    if (!computed->expr()->Is<googlesql::ResolvedAggregateFunctionCall>() || !type) {
      return std::nullopt;
    }
    const auto sql = NonScalarCall(*call, call, "", scope, input->columns);
    if (!sql) {
      return std::nullopt;
    }
    const int id = computed->column().column_id();
    projections.push_back("CAST(" + *sql + " AS " + *type + ") AS " + ColumnName(id));
    result.columns.emplace(id, "q." + ColumnName(id));
  }
  if (projections.empty()) {
    return std::nullopt;
  }
  result.sql = "SELECT " + Join(projections, ", ") + input->From() +
               (keys.empty() ? "" : " GROUP BY " + Join(keys, ", "));
  return result;
}

std::optional<Relation> AnalyticScan(const googlesql::ResolvedAnalyticScan& analytic,
                                     const Scope& scope) {
  auto result = Scan(*analytic.input_scan(), scope);
  if (!result) {
    return std::nullopt;
  }
  const Columns input_columns = result->columns;
  std::vector<std::string> projections = {"q.*"};
  for (const auto& group : analytic.function_group_list()) {
    for (const auto& computed : group->analytic_function_list()) {
      if (!computed->expr()->Is<googlesql::ResolvedAnalyticFunctionCall>()) {
        return std::nullopt;
      }
      const auto* call = computed->expr()->GetAs<googlesql::ResolvedAnalyticFunctionCall>();
      const auto type = SqlType(call->type());
      const auto over = Window(*group, call->window_frame(), scope, input_columns);
      if (!type || !over) {
        return std::nullopt;
      }
      const auto sql = NonScalarCall(*call, nullptr, *over, scope, input_columns);
      if (!sql) {
        return std::nullopt;
      }
      const int id = computed->column().column_id();
      projections.push_back("CAST(" + *sql + " AS " + *type + ") AS " + ColumnName(id));
      result->columns.emplace(id, "q." + ColumnName(id));
    }
  }
  result->sql = "SELECT " + Join(projections, ", ") + result->From();
  return result;
}

std::optional<Relation> WithScan(const googlesql::ResolvedWithScan& with, const Scope& scope) {
  // DuckDB's recursive CTEs differ in how they terminate and deduplicate.
  if (with.recursive()) {
    return std::nullopt;
  }
  Scope inner = scope;
  std::vector<std::string> definitions;
  for (const auto& entry : with.with_entry_list()) {
    const auto* subquery = entry->with_subquery();
    const auto relation = Scan(*subquery, inner);
    if (!relation) {
      return std::nullopt;
    }
    std::vector<std::string> names;
    names.reserve(subquery->column_list_size());
    for (int i = 0; i < subquery->column_list_size(); ++i) {
      names.push_back(QuoteIdentifier("_p" + std::to_string(i)));
    }
    const auto projections = Renamed(*relation, subquery->column_list(), names);
    if (!projections || projections->empty()) {
      return std::nullopt;
    }
    const std::string name = "_w" + std::to_string(scope.next_name++);
    definitions.push_back(QuoteIdentifier(name) + " AS (SELECT " + Join(*projections, ", ") +
                          relation->From() + ")");
    inner.with[entry->with_query_name()] = WithQuery{name, names.size()};
  }
  auto result = Scan(*with.query(), inner);
  if (!result) {
    return std::nullopt;
  }
  result->sql = "WITH " + Join(definitions, ", ") + " SELECT q.*" + result->From();
  return result;
}

std::optional<Relation> WithRefScan(const googlesql::ResolvedWithRefScan& ref, const Scope& scope) {
  const auto with = scope.with.find(ref.with_query_name());
  if (with == scope.with.end() ||
      with->second.width != static_cast<size_t>(ref.column_list_size())) {
    return std::nullopt;
  }
  Relation result;
  std::vector<std::string> projections;
  for (int i = 0; i < ref.column_list_size(); ++i) {
    const int id = ref.column_list(i).column_id();
    projections.push_back("q." + QuoteIdentifier("_p" + std::to_string(i)) + " AS " +
                          ColumnName(id));
    result.columns.emplace(id, "q." + ColumnName(id));
  }
  result.sql =
      "SELECT " + Join(projections, ", ") + " FROM " + QuoteIdentifier(with->second.name) + " AS q";
  return result;
}

std::optional<Relation> SetOperationScan(const googlesql::ResolvedSetOperationScan& set,
                                         const Scope& scope) {
  if (set.column_match_mode() != googlesql::ResolvedSetOperationScan::BY_POSITION ||
      set.column_propagation_mode() != googlesql::ResolvedSetOperationScan::STRICT) {
    return std::nullopt;
  }
  static const std::map<googlesql::ResolvedSetOperationScan::SetOperationType, std::string>
      operators = {{googlesql::ResolvedSetOperationScan::UNION_ALL, " UNION ALL "},
                   {googlesql::ResolvedSetOperationScan::UNION_DISTINCT, " UNION "},
                   {googlesql::ResolvedSetOperationScan::INTERSECT_ALL, " INTERSECT ALL "},
                   {googlesql::ResolvedSetOperationScan::INTERSECT_DISTINCT, " INTERSECT "},
                   {googlesql::ResolvedSetOperationScan::EXCEPT_ALL, " EXCEPT ALL "},
                   {googlesql::ResolvedSetOperationScan::EXCEPT_DISTINCT, " EXCEPT "}};
  const auto op = operators.find(set.op_type());
  if (op == operators.end() || set.column_list().empty()) {
    return std::nullopt;
  }
  Relation result;
  std::vector<std::string> names;
  for (const auto& column : set.column_list()) {
    names.push_back(ColumnName(column.column_id()));
    result.columns.emplace(column.column_id(), "q." + names.back());
  }
  std::vector<std::string> parts;
  for (const auto& item : set.input_item_list()) {
    const auto relation = Scan(*item->scan(), scope);
    if (!relation || item->output_column_list().size() != names.size()) {
      return std::nullopt;
    }
    const auto projections = Renamed(*relation, item->output_column_list(), names);
    if (!projections) {
      return std::nullopt;
    }
    parts.push_back("SELECT " + Join(*projections, ", ") + relation->From());
  }
  result.sql = Join(parts, op->second);
  return result;
}

std::optional<Relation> ArrayScan(const googlesql::ResolvedArrayScan& array, const Scope& scope) {
  if (array.array_expr_list_size() != 1 || array.element_column_list_size() != 1 ||
      array.array_zip_mode() != nullptr) {
    return std::nullopt;
  }
  auto result = array.input_scan() == nullptr
                    ? std::optional<Relation>(Relation{"SELECT 1 AS _unit", {}, {}})
                    : Scan(*array.input_scan(), scope);
  if (!result) {
    return std::nullopt;
  }
  const auto elements = Expression(*array.array_expr_list(0), scope, result->columns);
  if (!elements) {
    return std::nullopt;
  }
  // UNNEST is joined laterally so the array can refer to the input row. Ordinals are 1-based.
  Columns visible = result->columns;
  std::vector<std::string> projections = {"q.*"};
  const int element = array.element_column_list(0).column_id();
  projections.push_back("u.e AS " + ColumnName(element));
  visible.emplace(element, "u.e");
  result->columns.emplace(element, "q." + ColumnName(element));
  if (array.array_offset_column() != nullptr) {
    const int offset = array.array_offset_column()->column().column_id();
    projections.push_back("CAST(u.o - 1 AS BIGINT) AS " + ColumnName(offset));
    visible.emplace(offset, "(u.o - 1)");
    result->columns.emplace(offset, "q." + ColumnName(offset));
  }
  std::string condition = "TRUE";
  if (array.join_expr() != nullptr) {
    const auto on = Expression(*array.join_expr(), scope, visible);
    if (!on) {
      return std::nullopt;
    }
    condition = *on;
  }
  result->sql = "SELECT " + Join(projections, ", ") + result->From() +
                (array.is_outer() ? " LEFT" : " INNER") + " JOIN LATERAL unnest(" + *elements +
                ") WITH ORDINALITY AS u(e, o) ON " + condition;
  return result;
}

std::optional<Relation> ScanBody(const googlesql::ResolvedScan& scan, const Scope& scope) {
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
  if (scan.Is<googlesql::ResolvedJoinScan>()) {
    return JoinScan(*scan.GetAs<googlesql::ResolvedJoinScan>(), scope);
  }
  if (scan.Is<googlesql::ResolvedAggregateScan>()) {
    return AggregateScan(*scan.GetAs<googlesql::ResolvedAggregateScan>(), scope);
  }
  if (scan.Is<googlesql::ResolvedAnalyticScan>()) {
    return AnalyticScan(*scan.GetAs<googlesql::ResolvedAnalyticScan>(), scope);
  }
  if (scan.Is<googlesql::ResolvedWithScan>()) {
    return WithScan(*scan.GetAs<googlesql::ResolvedWithScan>(), scope);
  }
  if (scan.Is<googlesql::ResolvedWithRefScan>()) {
    return WithRefScan(*scan.GetAs<googlesql::ResolvedWithRefScan>(), scope);
  }
  if (scan.Is<googlesql::ResolvedSetOperationScan>()) {
    return SetOperationScan(*scan.GetAs<googlesql::ResolvedSetOperationScan>(), scope);
  }
  if (scan.Is<googlesql::ResolvedArrayScan>()) {
    return ArrayScan(*scan.GetAs<googlesql::ResolvedArrayScan>(), scope);
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
  auto result = Scan(*input, scope);
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
      const auto expression = Expression(*computed->expr(), scope, input_columns);
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
                                   scope, result->columns);
    if (!filter) {
      return std::nullopt;
    }
    result->sql = "SELECT q.*" + from + " WHERE " + *filter;
  } else if (scan.Is<googlesql::ResolvedOrderByScan>()) {
    const auto order = OrderItems(
        scan.GetAs<googlesql::ResolvedOrderByScan>()->order_by_item_list(), scope, result->columns);
    if (!order) {
      return std::nullopt;
    }
    result->ordering = {*order};
  } else {
    const auto* limit = scan.GetAs<googlesql::ResolvedLimitOffsetScan>();
    const auto count = Expression(*limit->limit(), scope, {});
    if (!count) {
      return std::nullopt;
    }
    result->sql = "SELECT q.*" + from + result->Order() + " LIMIT " + *count;
    if (limit->offset() != nullptr) {
      const auto offset = Expression(*limit->offset(), scope, {});
      if (!offset) {
        return std::nullopt;
      }
      result->sql += " OFFSET " + *offset;
    }
  }
  return result;
}

std::optional<Relation> Scan(const googlesql::ResolvedScan& scan, const Scope& scope) {
  if (!scan.hint_list().empty()) {
    return std::nullopt;
  }
  auto result = ScanBody(scan, scope);
  // Ordering from a subquery is not a promise of ordering for its parent scan.
  if (result && !scan.is_ordered()) {
    result->ordering.clear();
  }
  return result;
}

std::optional<std::string> Insert(const googlesql::ResolvedInsertStmt& insert, const Scope& scope) {
  const auto* table = insert.table_scan();
  if (insert.insert_mode() != googlesql::ResolvedInsertStmt::OR_ERROR ||
      insert.assert_rows_modified() != nullptr || insert.returning() != nullptr ||
      insert.on_conflict_clause() != nullptr || insert.query_parameter_list_size() != 0 ||
      insert.generated_column_expr_list_size() != 0 ||
      insert.timestamp_version_column() != nullptr || insert.temporal_at() != nullptr ||
      !table->hint_list().empty() || table->for_system_time_expr() != nullptr ||
      table->table()->IsValueTable() ||
      table->column_list_size() != table->column_index_list_size()) {
    return std::nullopt;
  }
  // The inserted columns are the table scan's columns; name them by the table's own columns.
  std::map<int, std::string> table_columns;
  for (int i = 0; i < table->column_list_size(); ++i) {
    table_columns.emplace(
        table->column_list(i).column_id(),
        QuoteIdentifier(table->table()->GetColumn(table->column_index_list(i))->Name()));
  }
  std::vector<std::string> names;
  for (const auto& column : insert.insert_column_list()) {
    const auto name = table_columns.find(column.column_id());
    if (name == table_columns.end() || !SqlType(column.type()) ||
        column.type_annotation_map() != nullptr) {
      return std::nullopt;
    }
    names.push_back(name->second);
  }
  std::string sql = "INSERT INTO " + QuoteIdentifierPath(table->table()->FullName()) + " (" +
                    Join(names, ", ") + ") ";
  if (insert.query() != nullptr) {
    const auto relation = Scan(*insert.query(), scope);
    if (!relation) {
      return std::nullopt;
    }
    std::vector<std::string> projections;
    for (const auto& output : insert.query_output_column_list()) {
      const auto column = relation->columns.find(output.column_id());
      if (column == relation->columns.end()) {
        return std::nullopt;
      }
      projections.push_back(column->second);
    }
    return sql + "SELECT " + Join(projections, ", ") + relation->From();
  }
  if (insert.row_list_size() == 0) {
    return std::nullopt;
  }
  std::vector<std::string> rows;
  for (const auto& row : insert.row_list()) {
    std::vector<std::string> values;
    for (const auto& dml_value : row->value_list()) {
      const auto* value = dml_value->value();
      if (value->Is<googlesql::ResolvedDMLDefault>()) {
        values.emplace_back("DEFAULT");
        continue;
      }
      const auto sql_value = Expression(*value, scope, {});
      if (!sql_value) {
        return std::nullopt;
      }
      values.push_back(*sql_value);
    }
    rows.push_back("(" + Join(values, ", ") + ")");
  }
  return sql + "VALUES " + Join(rows, ", ");
}

}  // namespace

std::optional<std::string> TranslateResolvedToDuckDbSql(
    const googlesql::ResolvedStatement& statement, const QueryParameters& parameters) {
  if (!statement.hint_list().empty()) {
    return std::nullopt;
  }
  int next_name = 0;
  const Scope scope{parameters, next_name, {}, {}};
  if (statement.Is<googlesql::ResolvedInsertStmt>()) {
    return Insert(*statement.GetAs<googlesql::ResolvedInsertStmt>(), scope);
  }
  if (!statement.Is<googlesql::ResolvedQueryStmt>()) {
    return std::nullopt;
  }
  const auto* query = statement.GetAs<googlesql::ResolvedQueryStmt>();
  if (query->is_value_table()) {
    return std::nullopt;
  }
  const auto relation = Scan(*query->query(), scope);
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
