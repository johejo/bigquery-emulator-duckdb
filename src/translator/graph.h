#pragma once

#include <optional>
#include <string>

#include "src/translator/context.h"

namespace googlesql {
class ResolvedCreatePropertyGraphStmt;
class ResolvedGraphTableScan;
class ResolvedGraphScan;
}  // namespace googlesql

namespace bigquery_emulator_duckdb::translator {
std::optional<std::string> CreatePropertyGraph(
    const googlesql::ResolvedCreatePropertyGraphStmt& create, const Scope& scope);
std::optional<Relation> GraphScan(const googlesql::ResolvedGraphScan& scan, const Scope& scope);
std::optional<Relation> GraphTableScan(const googlesql::ResolvedGraphTableScan& scan,
                                       const Scope& scope);
}  // namespace bigquery_emulator_duckdb::translator
