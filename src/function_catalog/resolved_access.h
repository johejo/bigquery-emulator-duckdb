#pragma once

// What the function registry reads from the resolved AST: arguments, types and the values of
// literals. The registry and its handlers do not depend on the AST definitions, so this is the
// only place they reach the AST from.

#include <cstddef>
#include <optional>
#include <span>
#include <string>

namespace googlesql {
class ResolvedAggregateFunctionCall;
class ResolvedExpr;
class ResolvedFunctionCallBase;
}  // namespace googlesql

namespace bigquery_emulator_duckdb::function_catalog {

struct TypeRejection;

// Raise the registered BigQuery error when the resolved argument types match a rejection.
void RejectArgumentTypes(const googlesql::ResolvedFunctionCallBase& call,
                         std::span<const TypeRejection> rejections);

// Whether an aggregate orders solely by its argument, which must be a column reference.
bool OrdersOnlyArgument(const googlesql::ResolvedAggregateFunctionCall& call, std::size_t argument);

// Whether `expr` is a literal or a query parameter, as BigQuery requires of some arguments.
bool IsConstant(const googlesql::ResolvedExpr& expr);

// Whether an expression is constant, including immutable calls and casts. No value means
// GoogleSQL could not determine it; callers leave that argument form unsupported.
std::optional<bool> IsConstantExpression(const googlesql::ResolvedExpr& expr);

// Whether an expression is a NULL literal, including a typed NULL.
bool IsNullLiteral(const googlesql::ResolvedExpr& expr);

// The value of a STRING literal that is not NULL.
std::optional<std::string> StringLiteral(const googlesql::ResolvedExpr& expr);

// Date parts are enum literals after analysis, not SQL identifier expressions.
std::optional<std::string> DatePart(const googlesql::ResolvedExpr& expr);

// The name of a rounding mode, such as ROUND_HALF_EVEN, which is also an enum literal after
// analysis.
std::optional<std::string> RoundingModeOf(const googlesql::ResolvedExpr& expr);

// The name of a NORMALIZE mode, such as NFKC.
std::optional<std::string> NormalizeMode(const googlesql::ResolvedExpr& expr);

}  // namespace bigquery_emulator_duckdb::function_catalog
