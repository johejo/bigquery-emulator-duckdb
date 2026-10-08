#pragma once

#include <optional>
#include <string>

#include "src/translator/context.h"

namespace googlesql {
class ResolvedScan;
class Type;
}  // namespace googlesql

namespace bigquery_emulator_duckdb::translator {

// Scans, in scan.cc.

std::optional<Relation> Scan(const googlesql::ResolvedScan& scan, const Scope& scope);

// Reads `column`, a table column of `type`, as the translator's other expressions of `type` are
// typed. A BIGNUMERIC(P, S) column, at any depth, has a DuckDB type of its own, which DuckDB
// casts to BIGNUM but not to another BIGNUMERIC(P, S) column's type.
std::string StoredColumn(const std::string& column, const googlesql::Type* type);

}  // namespace bigquery_emulator_duckdb::translator
