#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "src/translator/context.h"

namespace googlesql {
class ResolvedScan;
class ResolvedTableScan;
class Type;
}  // namespace googlesql

namespace bigquery_emulator_duckdb::translator {

// Scans, in scan.cc.

std::optional<Relation> Scan(const googlesql::ResolvedScan& scan, const Scope& scope);

// The DuckDB query of the rows the table of `scan` had at the time FOR SYSTEM_TIME AS OF gives,
// which `time` is set to, in microseconds since the Unix epoch.
std::optional<std::string> TimeTravelRows(const googlesql::ResolvedTableScan& scan,
                                          const Scope& scope, int64_t& time);

// Reads `column`, a table column of `type`, as the translator's other expressions of `type` are
// typed. A BIGNUMERIC(P, S) column, at any depth, has a DuckDB type of its own, which DuckDB
// casts to BIGNUM but not to another BIGNUMERIC(P, S) column's type.
std::string StoredColumn(const std::string& column, const googlesql::Type* type);

}  // namespace bigquery_emulator_duckdb::translator
