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

// Expressions, in expression.cc.

std::optional<std::string> Expression(const googlesql::ResolvedExpr& expr, const Scope& scope,
                                      const Columns& columns);

// The scope of a subquery or lateral join, whose correlated references see `columns`.
Scope Nested(const Scope& scope, const Columns& columns);

// The BIGINT that compares and orders the INTERVAL `sql` as GoogleSQL does the value.
std::string IntervalKey(const std::string& sql);

// The value that compares and groups `sql`, of `type`, as GoogleSQL does the value: INTERVAL
// values, also in STRUCT fields and ARRAY elements, become their keys. Other values stay as they
// are.
std::string IntervalKey(const googlesql::Type* type, const std::string& sql);

// ORDER BY items, with BigQuery's default NULL ordering spelled out. `offset_range_frame` marks
// the items that order a RANGE window frame with an offset bound.
std::optional<std::string> OrderItems(
    const std::vector<std::unique_ptr<const googlesql::ResolvedOrderByItem>>& items,
    const Scope& scope, const Columns& columns, bool offset_range_frame = false);

}  // namespace bigquery_emulator_duckdb::translator
