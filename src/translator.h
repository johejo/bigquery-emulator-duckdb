#pragma once

#include <string>

#include "src/frontend.h"

namespace bigquery_emulator_duckdb {

// Converts GoogleSQL into DuckDB SQL.
//
// The translation unparses the parser AST produced by the frontend, replacing the constructs
// whose spelling differs in DuckDB (identifiers, literals, type names, STRUCT constructors and
// a few functions). The output is normalised SQL rather than the original text, and comments
// are dropped, because they are not part of the AST.
std::string TranslateToDuckDbSql(const FrontendResult& frontend_result);

}  // namespace bigquery_emulator_duckdb
