#pragma once

#include <optional>
#include <string>

namespace googlesql {
class ResolvedStatement;
}

namespace bigquery_emulator_duckdb::translator {
struct Scope;

// Dispatches a resolved statement to its translator and projects query outputs.
std::optional<std::string> Statement(const googlesql::ResolvedStatement& statement,
                                     const Scope& scope);

// JobStatistics2.statementType of a statement the translator supports.
std::string StatementType(const googlesql::ResolvedStatement& statement);

}  // namespace bigquery_emulator_duckdb::translator
