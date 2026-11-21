#pragma once

#include <optional>

#include "src/translator/context.h"

// The resolved AST header takes about 30s to parse, and this header needs only the name.
namespace googlesql {
class ResolvedTVFScan;
}  // namespace googlesql

namespace bigquery_emulator_duckdb::translator {

// BigQuery's GAP_FILL table function, in gap_fill.cc.
std::optional<Relation> GapFillScan(const googlesql::ResolvedTVFScan& tvf, const Scope& scope);

}  // namespace bigquery_emulator_duckdb::translator
