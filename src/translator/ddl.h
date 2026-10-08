#pragma once

#include <optional>
#include <string>

#include "src/translator/context.h"

namespace googlesql {
class ResolvedAlterSchemaStmt;
class ResolvedAlterTableSetOptionsStmt;
class ResolvedAlterTableStmt;
class ResolvedCreateSchemaStmt;
class ResolvedCreateTableAsSelectStmt;
class ResolvedCreateTableStmt;
class ResolvedCreateViewStmt;
class ResolvedDropStmt;
}  // namespace googlesql

namespace bigquery_emulator_duckdb::translator {

// DDL, in ddl.cc.

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

}  // namespace bigquery_emulator_duckdb::translator
