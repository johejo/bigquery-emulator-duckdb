#pragma once

#include <optional>
#include <string>

#include "src/translator/context.h"

namespace googlesql {
class ResolvedAlterSchemaStmt;
class ResolvedAlterTableSetOptionsStmt;
class ResolvedAlterTableStmt;
class ResolvedCreateFunctionStmt;
class ResolvedCreateSchemaStmt;
class ResolvedCreateTableAsSelectStmt;
class ResolvedCreateTableStmt;
class ResolvedCreateViewStmt;
class ResolvedDropFunctionStmt;
class ResolvedDropStmt;
}  // namespace googlesql

namespace bigquery_emulator_duckdb::translator {

// DDL entry points, implemented by object kind in ddl_*.cc.

std::optional<std::string> CreateTable(const googlesql::ResolvedCreateTableStmt& create,
                                       const Scope& scope);
std::optional<std::string> CreateTableAsSelect(
    const googlesql::ResolvedCreateTableAsSelectStmt& create, const Scope& scope);
std::optional<std::string> AlterTable(const googlesql::ResolvedAlterTableStmt& alter,
                                      const Scope& scope);
std::optional<std::string> AlterTableSetOptions(
    const googlesql::ResolvedAlterTableSetOptionsStmt& alter, const Scope& scope);
std::optional<std::string> CreateView(const googlesql::ResolvedCreateViewStmt& create,
                                      const Scope& scope);
std::optional<std::string> CreateSchema(const googlesql::ResolvedCreateSchemaStmt& create,
                                        const Scope& scope);
std::optional<std::string> AlterSchema(const googlesql::ResolvedAlterSchemaStmt& alter,
                                       const Scope& scope);
std::optional<std::string> Drop(const googlesql::ResolvedDropStmt& drop, const Scope& scope);
// A persistent SQL UDF, which the emulator records on a DuckDB macro of its dataset; see Routine.
std::optional<std::string> CreateFunction(const googlesql::ResolvedCreateFunctionStmt& create,
                                          const Scope& scope);
std::optional<std::string> DropFunction(const googlesql::ResolvedDropFunctionStmt& drop,
                                        const Scope& scope);

}  // namespace bigquery_emulator_duckdb::translator
