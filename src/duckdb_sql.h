#pragma once

#include <string>
#include <string_view>

namespace bigquery_emulator_duckdb {

std::string ToUpperAscii(std::string_view text);

// '...' with the DuckDB escaping rules: a single quote is doubled and nothing else is special.
std::string QuoteLiteral(std::string_view value);

// "..." with the DuckDB escaping rules: a double quote is doubled.
std::string QuoteIdentifier(std::string_view name);

// A backtick quoted GoogleSQL identifier holds a whole path in BigQuery: `project.dataset.table`
// is one identifier that names three objects, so it becomes "project"."dataset"."table".
std::string QuoteIdentifierPath(std::string_view path);

std::string ToHex(std::string_view value);

// Maps a GoogleSQL type name onto its DuckDB spelling, keeping the original name when the two
// agree. `has_type_parameters` drops the default parameters of the mapped name, because an
// explicit parameter list follows and replaces them (NUMERIC(10) -> DECIMAL(10)).
std::string DuckDbTypeName(std::string_view googlesql_name, bool has_type_parameters = false);

}  // namespace bigquery_emulator_duckdb
