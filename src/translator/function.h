#pragma once

#include <optional>
#include <string>

#include "src/translator/context.h"

namespace googlesql {
class ResolvedFunctionCall;
class ResolvedFunctionCallBase;
class Type;
}  // namespace googlesql

namespace bigquery_emulator_duckdb::translator {

// Scalar functions and operators, in function.cc.

// Whether `type` is BIGNUMERIC or holds one, at any depth.
bool HasBigNumeric(const googlesql::Type* type);

// Whether the result or an argument of `call` has a BIGNUMERIC, at any depth.
bool InvolvesBigNumeric(const googlesql::ResolvedFunctionCallBase& call);

std::optional<std::string> Function(const googlesql::ResolvedFunctionCall& call, const Scope& scope,
                                    const Columns& columns);

}  // namespace bigquery_emulator_duckdb::translator
