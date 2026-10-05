#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "googlesql/public/type.h"
#include "googlesql/resolved_ast/resolved_ast.h"
#include "src/duckdb_sql.h"
#include "src/translator/internal.h"

namespace bigquery_emulator_duckdb::translator {
namespace {

// A cast of `sql` between scalar types `from` and `to` by GoogleSQL's conversion, where DuckDB's
// cast differs: DuckDB writes numbers and times in other formats, such as a DECIMAL padded with
// zeros to its scale, and reads dates and times in more formats than BigQuery. `safe` makes a
// conversion's errors NULL.
std::optional<std::string> ConvertedCast(const googlesql::Type* from, const googlesql::Type* to,
                                         const std::string& sql, bool safe) {
  if (to->IsString()) {
    if (from->IsNumericType()) {
      return "bq_decimal_string(CAST(" + sql + " AS VARCHAR))";
    }
    if (from->IsDouble()) {
      return "bq_double_string(" + sql + ")";
    }
    if (from->IsDatetime()) {
      return "bq_datetime_string(" + sql + ")";
    }
    if (from->IsTime()) {
      return "bq_time_string(" + sql + ")";
    }
    if (from->IsTimestamp()) {
      return "bq_timestamp_string(" + sql + ")";
    }
  }
  if (from->IsString()) {
    const std::string arguments = "(" + sql + ", " + (safe ? "true" : "false") + ")";
    if (to->IsDate()) {
      return "bq_string_to_date" + arguments;
    }
    if (to->IsDatetime()) {
      return "bq_string_to_datetime" + arguments;
    }
    if (to->IsTime()) {
      return "bq_string_to_time" + arguments;
    }
    if (to->IsTimestamp()) {
      return "bq_string_to_timestamp" + arguments;
    }
  }
  return std::nullopt;
}

// A cast of `sql` from `from` to `to`. A BIGNUMERIC is cast by GoogleSQL's conversions, which make
// their errors NULL under SAFE_CAST, where DuckDB would cast a BIGNUM through DOUBLE or truncate a
// string. Arrays and structs are cast element by element and field by field, structs by position
// as BigQuery does, and their other values as DuckDB casts them, except where ConvertedCast
// applies. Under SAFE_CAST, a value that fails makes its whole array or struct NULL, which its
// parent then counts as failed.
std::optional<std::string> CastValue(const googlesql::Type* from, const googlesql::Type* to,
                                     const std::string& sql, bool safe, Context& context) {
  if (from->Equals(to)) {
    return sql;
  }
  if (from->IsArray() && to->IsArray()) {
    const std::string element = context.FreshName("_e");
    const auto cast = CastValue(from->AsArray()->element_type(), to->AsArray()->element_type(),
                                element, safe, context);
    if (!cast) {
      return std::nullopt;
    }
    if (!safe) {
      return "list_transform(" + sql + ", " + element + " -> " + *cast + ")";
    }
    // A value cast to NULL from a value that is not NULL failed.
    const std::string array = context.FreshName("_a");
    const std::string casts = context.FreshName("_c");
    return "list_transform([" + sql + "], " + array + " -> list_transform([list_transform(" +
           array + ", " + element + " -> " + *cast + ")], " + casts + " -> CASE WHEN list_count(" +
           casts + ") < list_count(" + array + ") THEN NULL ELSE " + casts + " END)[1])[1]";
  }
  if (from->IsStruct() && to->IsStruct()) {
    const std::string value = context.FreshName("_s");
    const std::string casts = context.FreshName("_c");
    std::vector<std::string> fields;
    std::vector<std::string> failures;
    for (int i = 0; i < to->AsStruct()->num_fields(); ++i) {
      const auto cast = CastValue(from->AsStruct()->field(i).type, to->AsStruct()->field(i).type,
                                  "struct_extract_at(" + value + ", " + std::to_string(i + 1) + ")",
                                  safe, context);
      if (!cast) {
        return std::nullopt;
      }
      fields.push_back(QuoteIdentifier(to->AsStruct()->field(i).name) + " := " + *cast);
      std::string failure =
          "(struct_extract_at(" + casts + ", " + std::to_string(i + 1) + ") IS NULL AND ";
      failure += "struct_extract_at(" + value + ", " + std::to_string(i + 1) + ") IS NOT NULL)";
      failures.push_back(std::move(failure));
    }
    const std::string fields_sql = "struct_pack(" + Join(fields, ", ") + ")";
    const std::string cast_sql = safe ? "list_transform([" + fields_sql + "], " + casts +
                                            " -> CASE WHEN " + Join(failures, " OR ") +
                                            " THEN NULL ELSE " + casts + " END)[1]"
                                      : fields_sql;
    return "list_transform([" + sql + "], " + value + " -> CASE WHEN " + value +
           " IS NULL THEN NULL ELSE " + cast_sql + " END)[1]";
  }
  const std::string flag = safe ? "true" : "false";
  if (to->IsBigNumericType()) {
    if (from->IsString() || from->IsInt64() || from->IsNumericType()) {
      return "CAST(bq_bignumeric_from_string(CAST(" + sql + " AS VARCHAR), " + flag +
             ") AS BIGNUM)";
    }
    if (from->IsDouble()) {
      return "CAST(bq_bignumeric_from_double(" + sql + ", " + flag + ") AS BIGNUM)";
    }
    return std::nullopt;
  }
  if (!from->IsBigNumericType()) {
    if (const auto converted = ConvertedCast(from, to, sql, safe)) {
      return converted;
    }
    const auto type = DuckDbType(to);
    return type ? std::optional<std::string>(std::string(safe ? "TRY_CAST(" : "CAST(") + sql +
                                             " AS " + *type + ")")
                : std::nullopt;
  }
  const std::string units = "CAST(" + sql + " AS VARCHAR)";
  if (to->IsString()) {
    return "bq_bignumeric_to_string(" + units + ")";
  }
  if (to->IsNumericType()) {
    return "CAST(bq_bignumeric_to_numeric(" + units + ", " + flag + ") AS DECIMAL(38,9))";
  }
  if (to->IsInt64()) {
    return "bq_bignumeric_to_int64(" + units + ", " + flag + ")";
  }
  if (to->IsDouble()) {
    return "bq_bignumeric_to_double(" + units + ", " + flag + ")";
  }
  return std::nullopt;
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

}  // namespace

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

std::optional<std::string> Expression(const googlesql::ResolvedExpr& expr, const Scope& scope,
                                      const Columns& columns) {
  const auto type = DuckDbType(expr.type());
  if (!type || expr.type_annotation_map() != nullptr) {
    return Unsupported(scope, "type " + expr.type()->DebugString());
  }
  if (expr.Is<googlesql::ResolvedLiteral>()) {
    auto literal = Literal(expr.GetAs<googlesql::ResolvedLiteral>()->value());
    if (!literal) {
      return Unsupported(scope, "literal of type " + expr.type()->DebugString());
    }
    return literal;
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
    return parameter->name().empty() ? scope.context.parameters.ByPosition(parameter->position())
                                     : scope.context.parameters.ByName(parameter->name());
  }
  if (expr.Is<googlesql::ResolvedCast>()) {
    const auto* cast = expr.GetAs<googlesql::ResolvedCast>();
    if (cast->format() != nullptr || cast->time_zone() != nullptr ||
        cast->extended_cast() != nullptr || !cast->type_modifiers().IsEmpty()) {
      return Unsupported(scope, "CAST with FORMAT, time zone or type parameters");
    }
    const auto argument = Expression(*cast->expr(), scope, columns);
    if (!argument) {
      return std::nullopt;
    }
    const googlesql::Type* from = cast->expr()->type();
    if (!from->Equals(cast->type())) {
      const auto sql =
          CastValue(from, cast->type(), *argument, cast->return_null_on_error(), scope.context);
      return sql ? sql
                 : Unsupported(scope, "CAST from " + from->TypeName(googlesql::PRODUCT_EXTERNAL) +
                                          " to " +
                                          cast->type()->TypeName(googlesql::PRODUCT_EXTERNAL));
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
  if (expr.Is<googlesql::ResolvedGetJsonField>()) {
    const auto* get = expr.GetAs<googlesql::ResolvedGetJsonField>();
    const auto input = Expression(*get->expr(), scope, columns);
    if (!input) {
      return std::nullopt;
    }
    return "json(bq_json_field(CAST(" + *input + " AS VARCHAR), " +
           QuoteLiteral(get->field_name()) + "))";
  }
  if (expr.Is<googlesql::ResolvedSubqueryExpr>()) {
    return Subquery(*expr.GetAs<googlesql::ResolvedSubqueryExpr>(), *type, scope, columns);
  }
  return Unsupported(scope, "expression " + expr.node_kind_string());
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

}  // namespace bigquery_emulator_duckdb::translator
