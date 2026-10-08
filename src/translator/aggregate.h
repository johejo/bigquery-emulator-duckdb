#pragma once

#include <optional>
#include <string>

#include "src/translator/context.h"

namespace googlesql {
class ResolvedAggregateScan;
class ResolvedAnalyticScan;
}  // namespace googlesql

namespace bigquery_emulator_duckdb::translator {

// Aggregation and analytic functions, in aggregate.cc.

std::optional<Relation> AggregateScan(const googlesql::ResolvedAggregateScan& aggregate,
                                      const Scope& scope);
std::optional<Relation> AnalyticScan(const googlesql::ResolvedAnalyticScan& analytic,
                                     const Scope& scope);

}  // namespace bigquery_emulator_duckdb::translator
