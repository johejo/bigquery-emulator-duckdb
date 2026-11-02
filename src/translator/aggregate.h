#pragma once

#include <optional>
#include <string>

#include "src/translator/context.h"

namespace googlesql {
class ResolvedAggregateScan;
class ResolvedAnalyticScan;
// Keep resolved AST parsing out of callers that only dispatch array aggregates.
class ResolvedArrayAggregate;
}  // namespace googlesql

namespace bigquery_emulator_duckdb::translator {

// Aggregation and analytic functions, in aggregate.cc.

std::optional<std::string> ArrayAggregate(const googlesql::ResolvedArrayAggregate& aggregate,
                                          const Scope& scope, const Columns& columns);

std::optional<Relation> AggregateScan(const googlesql::ResolvedAggregateScan& aggregate,
                                      const Scope& scope);
std::optional<Relation> AnalyticScan(const googlesql::ResolvedAnalyticScan& analytic,
                                     const Scope& scope);

}  // namespace bigquery_emulator_duckdb::translator
