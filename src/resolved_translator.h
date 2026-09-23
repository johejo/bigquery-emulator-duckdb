#pragma once

#include <optional>
#include <string>

#include "src/query_parameters.h"

namespace googlesql {
class ResolvedStatement;
}

namespace bigquery_emulator_duckdb {

// Translates projections, table reads, filters, ordering and limits with supported
// scalar expressions. Columns are bound by resolved ID across scan scopes. nullopt means
// an unsupported construct, allowing the caller to translate the whole statement through
// the parser AST.
// Errors are not caught: a failed translation or execution must not trigger fallback.
// The statement's catalog and TypeFactory must remain alive for this call.
std::optional<std::string> TranslateResolvedToDuckDbSql(
    const googlesql::ResolvedStatement& statement, const QueryParameters& parameters = {});

}  // namespace bigquery_emulator_duckdb
