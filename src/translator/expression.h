#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "src/translator/context.h"

namespace googlesql {
class ResolvedExpr;
class ResolvedOrderByItem;
class Type;
}  // namespace googlesql

namespace bigquery_emulator_duckdb::translator {

// Whether a value, including its nested fields, uses positional internal struct names.
bool HasInternalStructNames(const googlesql::Type* type);

// Expressions, in expression.cc.

std::optional<std::string> Expression(const googlesql::ResolvedExpr& expr, const Scope& scope,
                                      const Columns& columns);

// The scope of a subquery or lateral join, whose correlated references see `columns`.
Scope Nested(const Scope& scope, const Columns& columns);

// ORDER BY items, with BigQuery's default NULL ordering spelled out.
std::optional<std::string> OrderItems(
    const std::vector<std::unique_ptr<const googlesql::ResolvedOrderByItem>>& items,
    const Scope& scope, const Columns& columns);

}  // namespace bigquery_emulator_duckdb::translator
