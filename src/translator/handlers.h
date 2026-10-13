#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "src/translator/functions.h"

namespace googlesql {
class ResolvedExpr;
class Type;
}  // namespace googlesql

namespace bigquery_emulator_duckdb::translator {

// Date parts are enum literals after analysis, not SQL identifier expressions.
std::optional<std::string> DatePart(const googlesql::ResolvedExpr& expr);

// A bucket width INTERVAL of a single part, as a count of months, days or microseconds.
// INTERVAL n PART resolves to $interval(n, PART), and an INTERVAL string to a literal.
struct BucketWidth {
  std::string unit;
  // The n of INTERVAL n PART, or null for a literal, whose count is all in `factor`.
  const googlesql::ResolvedExpr* count = nullptr;
  int64_t factor = 1;
};

std::optional<BucketWidth> BucketWidthOf(const googlesql::ResolvedExpr& expr);

// The handlers that the registry in functions.cc names.
std::optional<std::string> MakeArray(const ScalarCall& call);
std::optional<std::string> Logical(const ScalarCall& call);
std::optional<std::string> InList(const ScalarCall& call);
std::optional<std::string> Case(const ScalarCall& call);
std::optional<std::string> Bucket(const ScalarCall& call);
std::optional<std::string> ToJson(const ScalarCall& call);
std::optional<std::string> JsonRemove(const ScalarCall& call);
std::optional<std::string> JsonSet(const ScalarCall& call);
std::optional<std::string> JsonArrayModify(const ScalarCall& call);
std::optional<std::string> JsonArray(const ScalarCall& call);
std::optional<std::string> JsonObject(const ScalarCall& call);
std::optional<std::string> ArrayConcat(const ScalarCall& call);
std::optional<std::string> ConcatStrings(const ScalarCall& call);
std::optional<std::string> Format(const ScalarCall& call);
std::optional<std::string> Extremum(const ScalarCall& call);
std::optional<std::string> AddKeyFromRawBytes(const ScalarCall& call);

// Equality, IN, CASE with a value and NULLIF over STRUCTs, which BigQuery compares field by field
// with NULL fields propagating NULL; Call in function.cc takes these calls from their rules.
std::optional<std::string> StructComparison(const ScalarCall& call);
// `value` IN UNNEST(`array`) for a STRUCT `type`.
std::string StructInArray(const googlesql::Type* type, const std::string& value,
                          const std::string& array);

}  // namespace bigquery_emulator_duckdb::translator
