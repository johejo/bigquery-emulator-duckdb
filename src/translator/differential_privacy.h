#pragma once

#include <optional>

#include "src/translator/context.h"

// Keeps the resolved AST, which takes about 30s to parse, out of this header's includers.
namespace googlesql {
class ResolvedDifferentialPrivacyAggregateScan;
}  // namespace googlesql

namespace bigquery_emulator_duckdb::translator {

// SELECT WITH DIFFERENTIAL_PRIVACY, in differential_privacy.cc.

std::optional<Relation> DifferentialPrivacyAggregateScan(
    const googlesql::ResolvedDifferentialPrivacyAggregateScan& aggregate, const Scope& scope);

}  // namespace bigquery_emulator_duckdb::translator
