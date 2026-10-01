#pragma once

#include <string>
#include <string_view>

namespace bigquery_emulator_duckdb {

std::string ToUpperAscii(std::string_view text);
std::string ToLowerAscii(std::string_view text);

// '...' with the DuckDB escaping rules: a single quote is doubled and nothing else is special.
std::string QuoteLiteral(std::string_view value);

// "..." with the DuckDB escaping rules: a double quote is doubled.
std::string QuoteIdentifier(std::string_view name);

// Quotes a resolved table name as project.dataset.table. Domain-scoped project IDs may
// contain dots; only the last two dots delimit the dataset and table in that case.
std::string QuoteIdentifierPath(std::string_view path);

std::string ToHex(std::string_view value);

}  // namespace bigquery_emulator_duckdb
