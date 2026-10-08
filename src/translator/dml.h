#pragma once

#include <optional>
#include <string>

#include "src/translator/context.h"

namespace googlesql {
class ResolvedDeleteStmt;
class ResolvedInsertStmt;
class ResolvedMergeStmt;
class ResolvedTruncateStmt;
class ResolvedUpdateStmt;
}  // namespace googlesql

namespace bigquery_emulator_duckdb::translator {

// DML, in dml.cc.

std::optional<std::string> Insert(const googlesql::ResolvedInsertStmt& insert, const Scope& scope);
std::optional<std::string> Update(const googlesql::ResolvedUpdateStmt& update, const Scope& scope);
std::optional<std::string> Delete(const googlesql::ResolvedDeleteStmt& del, const Scope& scope);
std::optional<std::string> Truncate(const googlesql::ResolvedTruncateStmt& truncate,
                                    const Scope& scope);
std::optional<std::string> Merge(const googlesql::ResolvedMergeStmt& merge, const Scope& scope);

}  // namespace bigquery_emulator_duckdb::translator
