#pragma once

#include <cstddef>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "src/default_dataset.h"
#include "src/duckdb_sql.h"
#include "src/query_parameters.h"
#include "src/system_variables.h"

namespace bigquery_emulator_duckdb {
struct TranslatedStatement;
}  // namespace bigquery_emulator_duckdb

namespace bigquery_emulator_duckdb::translator {

// Each scan exposes synthetic names keyed by resolved column ID. User aliases only
// appear at the query boundary, so duplicate names and nested scopes cannot collide.
using Columns = std::map<int, std::string>;

inline std::string ColumnName(int id) { return QuoteIdentifier("_c" + std::to_string(id)); }

struct WithQuery {
  std::string name;
  size_t width;
};

// What the whole statement's translation shares: its inputs, and what it records on the way.
struct Context {
  const QueryParameters& parameters;
  const DefaultDataset& defaults;
  // DDL records what it creates, alters or drops in the statement itself, so that this header,
  // which every translator includes, does not need their definitions.
  TranslatedStatement& statement;
  // The values of the system variables of the script that the statement belongs to, if any.
  const googlesql::SystemVariableValuesMap* system_variables = nullptr;
  // Whether the statement defines a view, whose DuckDB view queries never read: they read the
  // view's GoogleSQL query, which the analyzer expands.
  bool defines_view = false;
  // The first construct found unsupported, reported in the error the statement fails with.
  std::string unsupported = {};
  int next_name = 0;

  // A name no other call returns, for WITH queries and lambda parameters.
  std::string FreshName(std::string_view prefix) {
    return std::string(prefix) + std::to_string(next_name++);
  }
};

// What a scan can see besides its own input: correlated columns of enclosing queries, which
// are referenced unqualified because every scope aliases its input as q, and named WITH queries.
// Nested scopes copy it; they all share one Context.
struct Relation;

struct Scope {
  Context& context;
  Columns outer = {};
  std::map<std::string, WithQuery> with = {};
  // The recursive query being defined, which a ResolvedRecursiveRefScan reads.
  std::optional<WithQuery> recursive = {};
  std::string graph_name = {};
  std::shared_ptr<Relation> graph_input = {};
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
  std::vector<std::string> ordering = {};

  [[nodiscard]] std::string From() const { return " FROM (" + sql + ") AS q"; }
  [[nodiscard]] std::string Order() const {
    return ordering.empty() ? "" : " ORDER BY " + Join(ordering, ", ");
  }
};

}  // namespace bigquery_emulator_duckdb::translator
