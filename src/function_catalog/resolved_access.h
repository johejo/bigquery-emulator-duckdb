#pragma once

// What the function registry reads from the resolved AST: arguments, types and the values of
// literals. The registry and its handlers do not depend on the AST definitions, so this is the
// only place they reach the AST from.

#include <cstdint>
#include <optional>
#include <string>

namespace googlesql {
class ResolvedExpr;
}  // namespace googlesql

namespace bigquery_emulator_duckdb::function_catalog {

// Whether `expr` is a literal or a query parameter, as BigQuery requires of some arguments.
bool IsConstant(const googlesql::ResolvedExpr& expr);

// The value of a STRING literal that is not NULL.
std::optional<std::string> StringLiteral(const googlesql::ResolvedExpr& expr);

// Date parts are enum literals after analysis, not SQL identifier expressions.
std::optional<std::string> DatePart(const googlesql::ResolvedExpr& expr);

// The name of a rounding mode, such as ROUND_HALF_EVEN, which is also an enum literal after
// analysis.
std::optional<std::string> RoundingMode(const googlesql::ResolvedExpr& expr);

// The name of a NORMALIZE mode, such as NFKC.
std::optional<std::string> NormalizeMode(const googlesql::ResolvedExpr& expr);

// A bucket width INTERVAL of a single part, as a count of months, days or microseconds.
// INTERVAL n PART resolves to $interval(n, PART), and an INTERVAL string to a literal.
struct BucketWidth {
  std::string unit;
  // The n of INTERVAL n PART, or null for a literal, whose count is all in `factor`.
  const googlesql::ResolvedExpr* count = nullptr;
  int64_t factor = 1;
};

std::optional<BucketWidth> BucketWidthOf(const googlesql::ResolvedExpr& expr);

}  // namespace bigquery_emulator_duckdb::function_catalog
