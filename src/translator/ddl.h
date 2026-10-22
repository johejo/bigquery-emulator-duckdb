#pragma once

#include <optional>
#include <string>

#include "src/translator/context.h"

namespace googlesql {
class ResolvedAlterSchemaStmt;
class ResolvedAlterTableSetOptionsStmt;
class ResolvedAlterTableStmt;
class ResolvedCreateFunctionStmt;
class ResolvedCreateTableFunctionStmt;
class ResolvedCreateProcedureStmt;
class ResolvedCreateSchemaStmt;
class ResolvedCreateSnapshotTableStmt;
class ResolvedCreateTableAsSelectStmt;
class ResolvedCreateTableStmt;
class ResolvedCreateViewBase;
class ResolvedDropFunctionStmt;
class ResolvedDropMaterializedViewStmt;
class ResolvedDropSnapshotTableStmt;
class ResolvedDropTableFunctionStmt;
class ResolvedDropStmt;
}  // namespace googlesql

namespace bigquery_emulator_duckdb::translator {

// DDL entry points, implemented by object kind in ddl_*.cc.

std::optional<std::string> CreateTable(const googlesql::ResolvedCreateTableStmt& create,
                                       const Scope& scope);
std::optional<std::string> CreateTableAsSelect(
    const googlesql::ResolvedCreateTableAsSelectStmt& create, const Scope& scope);
std::optional<std::string> CreateSnapshotTable(
    const googlesql::ResolvedCreateSnapshotTableStmt& create, const Scope& scope);
std::optional<std::string> AlterTable(const googlesql::ResolvedAlterTableStmt& alter,
                                      const Scope& scope);
std::optional<std::string> AlterTableSetOptions(
    const googlesql::ResolvedAlterTableSetOptionsStmt& alter, const Scope& scope);
std::optional<std::string> CreateView(const googlesql::ResolvedCreateViewBase& create,
                                      const Scope& scope);
std::optional<std::string> CreateSchema(const googlesql::ResolvedCreateSchemaStmt& create,
                                        const Scope& scope);
std::optional<std::string> AlterSchema(const googlesql::ResolvedAlterSchemaStmt& alter,
                                       const Scope& scope);
std::optional<std::string> Drop(const googlesql::ResolvedDropStmt& drop, const Scope& scope);
std::optional<std::string> DropMaterializedView(
    const googlesql::ResolvedDropMaterializedViewStmt& drop, const Scope& scope);
std::optional<std::string> DropSnapshotTable(const googlesql::ResolvedDropSnapshotTableStmt& drop,
                                             const Scope& scope);
// A persistent SQL UDF, which the emulator records on a DuckDB macro of its dataset; see Routine.
std::optional<std::string> CreateFunction(const googlesql::ResolvedCreateFunctionStmt& create,
                                          const Scope& scope);
std::optional<std::string> CreateTableFunction(
    const googlesql::ResolvedCreateTableFunctionStmt& create, const Scope& scope);
std::optional<std::string> CreateProcedure(const googlesql::ResolvedCreateProcedureStmt& create,
                                           const Scope& scope);
std::optional<std::string> DropTableFunction(const googlesql::ResolvedDropTableFunctionStmt& drop,
                                             const Scope& scope);
std::optional<std::string> DropProcedure(const googlesql::ResolvedDropStmt& drop,
                                         const Scope& scope);
std::optional<std::string> DropFunction(const googlesql::ResolvedDropFunctionStmt& drop,
                                        const Scope& scope);

}  // namespace bigquery_emulator_duckdb::translator
