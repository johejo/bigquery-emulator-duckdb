#pragma once

#include <optional>
#include <string>

namespace googlesql {
// Keep the resolved AST out of translation entry point headers.
class ResolvedExpr;
class ResolvedStatement;
}  // namespace googlesql

namespace bigquery_emulator_duckdb::translator {
// Avoid pulling translation state into users of these entry points.
struct Context;
struct Scope;

// Finds what reads of tables can skip before translating scans: the struct fields that are not
// read, and the filters that drop rows of each scan.
void CollectReadFields(const googlesql::ResolvedStatement& statement, Context& context);
void CollectReadFields(const googlesql::ResolvedExpr& expression, Context& context);

// Dispatches a resolved statement to its translator and projects query outputs.
std::optional<std::string> Statement(const googlesql::ResolvedStatement& statement,
                                     const Scope& scope);

// JobStatistics2.statementType of a statement the translator supports.
std::string StatementType(const googlesql::ResolvedStatement& statement);

}  // namespace bigquery_emulator_duckdb::translator
