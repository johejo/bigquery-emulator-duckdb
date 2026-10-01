#pragma once

// Shared by the translator's sources; not part of its interface, which is src/translator.h.

#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "src/duckdb_sql.h"
#include "src/query_parameters.h"
#include "src/translator.h"

namespace googlesql {
class ResolvedAlterTableStmt;
class ResolvedCreateSchemaStmt;
class ResolvedCreateTableAsSelectStmt;
class ResolvedCreateTableStmt;
class ResolvedCreateViewStmt;
class ResolvedDeleteStmt;
class ResolvedDropStmt;
class ResolvedExpr;
class ResolvedInsertStmt;
class ResolvedMergeStmt;
class ResolvedScan;
class ResolvedTruncateStmt;
class ResolvedUpdateStmt;
class Type;
class TypeParameters;
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

// What a scan can see besides its own input: correlated columns of enclosing queries, which
// are referenced unqualified because every scope aliases its input as q, and named WITH queries.
struct Scope {
  const QueryParameters& parameters;
  int& next_name;
  // The first construct found unsupported, reported in the error the statement fails with.
  std::string& unsupported;
  Columns outer;
  std::map<std::string, WithQuery> with;
  // The recursive query being defined, which a ResolvedRecursiveRefScan reads.
  std::optional<WithQuery> recursive;
};

// Records why the statement is unsupported. The innermost failure is recorded first, and the
// callers above it only propagate nullopt.
inline std::nullopt_t Unsupported(const Scope& scope, std::string_view what) {
  if (scope.unsupported.empty()) {
    scope.unsupported = what;
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

// Types and literals, in types.cc.

// The DuckDB type of `type`, narrowed by `parameters` when a column definition gives some.
// DuckDB ignores lengths, so STRING(L) and BYTES(L) lose them; NUMERIC(P, S) keeps its rounding
// as DECIMAL(P, S).
std::optional<std::string> SqlType(const googlesql::Type* type,
                                   const googlesql::TypeParameters* parameters = nullptr);

std::optional<std::string> Literal(const googlesql::Value& value);

// Queries and expressions, in translator.cc.

std::optional<Relation> Scan(const googlesql::ResolvedScan& scan, const Scope& scope);

std::optional<std::string> Expression(const googlesql::ResolvedExpr& expr, const Scope& scope,
                                      const Columns& columns);

// DML, in dml.cc.

std::optional<std::string> Insert(const googlesql::ResolvedInsertStmt& insert, const Scope& scope);
std::optional<std::string> Update(const googlesql::ResolvedUpdateStmt& update, const Scope& scope);
std::optional<std::string> Delete(const googlesql::ResolvedDeleteStmt& del, const Scope& scope);
std::optional<std::string> Truncate(const googlesql::ResolvedTruncateStmt& truncate,
                                    const Scope& scope);
std::optional<std::string> Merge(const googlesql::ResolvedMergeStmt& merge, const Scope& scope);

// DDL, in ddl.cc.

std::optional<std::string> CreateTable(const googlesql::ResolvedCreateTableStmt& create,
                                       const DefaultDataset& defaults, const Scope& scope);
std::optional<std::string> CreateTableAsSelect(
    const googlesql::ResolvedCreateTableAsSelectStmt& create, const DefaultDataset& defaults,
    const Scope& scope);
std::optional<std::string> AlterTable(const googlesql::ResolvedAlterTableStmt& alter,
                                      const DefaultDataset& defaults, const Scope& scope);
std::optional<std::string> CreateView(const googlesql::ResolvedCreateViewStmt& create,
                                      const DefaultDataset& defaults, const Scope& scope);
std::optional<std::string> CreateSchema(const googlesql::ResolvedCreateSchemaStmt& create,
                                        const DefaultDataset& defaults, const Scope& scope);
std::optional<std::string> Drop(const googlesql::ResolvedDropStmt& drop,
                                const DefaultDataset& defaults, const Scope& scope);

}  // namespace bigquery_emulator_duckdb::translator
