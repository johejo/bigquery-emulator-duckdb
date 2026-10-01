#pragma once

// Shared by the translator's sources; not part of its interface, which is src/translator.h.

#include <cstddef>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "src/duckdb_sql.h"
#include "src/query_parameters.h"
#include "src/translator.h"
#include "src/translator/functions.h"
#include "src/type_mapping.h"

namespace googlesql {
class ResolvedAlterTableStmt;
class ResolvedCreateSchemaStmt;
class ResolvedCreateTableAsSelectStmt;
class ResolvedCreateTableStmt;
class ResolvedCreateViewStmt;
class ResolvedDeleteStmt;
class ResolvedDropStmt;
class ResolvedAggregateScan;
class ResolvedAnalyticScan;
class ResolvedExpr;
class ResolvedFunctionCall;
class ResolvedOrderByItem;
class ResolvedInsertStmt;
class ResolvedMergeStmt;
class ResolvedScan;
class ResolvedTruncateStmt;
class ResolvedUpdateStmt;
class Value;
}  // namespace googlesql

namespace bigquery_emulator_duckdb::translator {

// Each scan exposes synthetic names keyed by resolved column ID. User aliases only
// appear at the query boundary, so duplicate names and nested scopes cannot collide.
using Columns = std::map<int, std::string>;

inline std::string ColumnName(int id) { return QuoteIdentifier("_c" + std::to_string(id)); }

inline std::string Join(const std::vector<std::string>& parts, std::string_view separator) {
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

// What the whole statement's translation shares: its inputs, and what it records on the way.
struct Context {
  const QueryParameters& parameters;
  const DefaultDataset& defaults;
  // The first construct found unsupported, reported in the error the statement fails with.
  std::string unsupported;
  // What DDL records about its target as it is translated.
  std::optional<TableReference> ddl_target_table;
  std::optional<DatasetReference> ddl_target_dataset;
  std::optional<TableDefinition> table;
  std::optional<AddedColumn> added_column;
  std::optional<ViewDefinition> view;
  int next_name = 0;

  // A name no other call returns, for WITH queries and lambda parameters.
  std::string FreshName(std::string_view prefix) {
    return std::string(prefix) + std::to_string(next_name++);
  }
};

// What a scan can see besides its own input: correlated columns of enclosing queries, which
// are referenced unqualified because every scope aliases its input as q, and named WITH queries.
// Nested scopes copy it; they all share one Context.
struct Scope {
  Context& context;
  Columns outer;
  std::map<std::string, WithQuery> with;
  // The recursive query being defined, which a ResolvedRecursiveRefScan reads.
  std::optional<WithQuery> recursive;
};

// Records why the statement is unsupported. The innermost failure is recorded first, and the
// callers above it only propagate nullopt.
inline std::nullopt_t Unsupported(const Scope& scope, std::string_view what) {
  if (scope.context.unsupported.empty()) {
    scope.context.unsupported = what;
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

// Literals, in literal.cc.

std::optional<std::string> Literal(const googlesql::Value& value);

// Expressions, in expression.cc.

std::optional<std::string> Expression(const googlesql::ResolvedExpr& expr, const Scope& scope,
                                      const Columns& columns);

// The scope of a subquery or lateral join, whose correlated references see `columns`.
Scope Nested(const Scope& scope, const Columns& columns);

// ORDER BY items, with BigQuery's default NULL ordering spelled out.
std::optional<std::string> OrderItems(
    const std::vector<std::unique_ptr<const googlesql::ResolvedOrderByItem>>& items,
    const Scope& scope, const Columns& columns);

// Scalar functions and operators, in function.cc.

std::optional<std::string> Function(const googlesql::ResolvedFunctionCall& call, const Scope& scope,
                                    const Columns& columns);

// A JSONPath key as DuckDB spells it, or nullopt for the empty key, which DuckDB rejects.
std::optional<std::string> JsonPathKey(std::string_view key);

// The handlers that the registry in functions.cc names, in function.cc.
std::optional<std::string> MakeArray(const ScalarCall& call);
std::optional<std::string> Logical(const ScalarCall& call);
std::optional<std::string> InList(const ScalarCall& call);
std::optional<std::string> Case(const ScalarCall& call);
std::optional<std::string> Bucket(const ScalarCall& call);
std::optional<std::string> RegexpExtract(const ScalarCall& call);
std::optional<std::string> JsonExtract(const ScalarCall& call);
std::optional<std::string> JsonSubscript(const ScalarCall& call);
std::optional<std::string> ToJson(const ScalarCall& call);
std::optional<std::string> JsonRemove(const ScalarCall& call);
std::optional<std::string> JsonSet(const ScalarCall& call);
std::optional<std::string> JsonObject(const ScalarCall& call);
std::optional<std::string> ArrayConcat(const ScalarCall& call);

// Scans, in scan.cc.

std::optional<Relation> Scan(const googlesql::ResolvedScan& scan, const Scope& scope);

// Aggregation and analytic functions, in aggregate.cc.

std::optional<Relation> AggregateScan(const googlesql::ResolvedAggregateScan& aggregate,
                                      const Scope& scope);
std::optional<Relation> AnalyticScan(const googlesql::ResolvedAnalyticScan& analytic,
                                     const Scope& scope);

// DML, in dml.cc.

std::optional<std::string> Insert(const googlesql::ResolvedInsertStmt& insert, const Scope& scope);
std::optional<std::string> Update(const googlesql::ResolvedUpdateStmt& update, const Scope& scope);
std::optional<std::string> Delete(const googlesql::ResolvedDeleteStmt& del, const Scope& scope);
std::optional<std::string> Truncate(const googlesql::ResolvedTruncateStmt& truncate,
                                    const Scope& scope);
std::optional<std::string> Merge(const googlesql::ResolvedMergeStmt& merge, const Scope& scope);

// DDL, in ddl.cc.

std::optional<std::string> CreateTable(const googlesql::ResolvedCreateTableStmt& create,
                                       const Scope& scope);
std::optional<std::string> CreateTableAsSelect(
    const googlesql::ResolvedCreateTableAsSelectStmt& create, const Scope& scope);
std::optional<std::string> AlterTable(const googlesql::ResolvedAlterTableStmt& alter,
                                      const Scope& scope);
std::optional<std::string> CreateView(const googlesql::ResolvedCreateViewStmt& create,
                                      const Scope& scope);
std::optional<std::string> CreateSchema(const googlesql::ResolvedCreateSchemaStmt& create,
                                        const Scope& scope);
std::optional<std::string> Drop(const googlesql::ResolvedDropStmt& drop, const Scope& scope);

}  // namespace bigquery_emulator_duckdb::translator
