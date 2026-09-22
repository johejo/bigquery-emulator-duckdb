#pragma once

#include <string>

#include "src/frontend.h"

namespace bigquery_emulator_duckdb {

// Converts GoogleSQL into DuckDB SQL.
//
// The current implementation is token based: it rewrites string literals, quoted identifiers,
// type names and a handful of functions, and passes everything else through unchanged. It will
// be replaced by a resolved AST based translation as the frontend grows.
std::string TranslateToDuckDbSql(const FrontendResult& frontend_result);

}  // namespace bigquery_emulator_duckdb
