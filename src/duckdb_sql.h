#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bigquery_emulator_duckdb {

std::string DuckPGQName(const std::vector<std::string>& path);

std::string ToUpperAscii(std::string_view text);
std::string ToLowerAscii(std::string_view text);

// A DuckDB string expression: quotes are doubled; embedded NULs use UTF-8 decoding.
std::string QuoteLiteral(std::string_view value);

// "..." with the DuckDB escaping rules: a double quote is doubled.
std::string QuoteIdentifier(std::string_view name);

// Reversible, case-sensitive physical names for BigQuery datasets and their relations.
// DuckDB's native schemas (including main and its internal tables) keep their names.
std::string DatasetCatalogName(std::string_view name);
std::string TableCatalogName(std::string_view dataset, std::string_view name);

// A DuckDB catalog relation with physical dataset and relation names decoded.
std::string LogicalCatalog(std::string_view relation);

// bq_schemas() restricted to the datasets of `project`, or to `dataset` when it is given, which
// it looks up by its physical name instead of scanning the catalog.
std::string SchemasQuery(std::string_view project, std::optional<std::string_view> dataset = {});

// bq_relations() restricted to the table or view `table` of `dataset` in `project`, which it looks
// up by its physical names instead of scanning the catalog: one row with its type, comment and
// table_oid, or none when it does not exist.
std::string RelationsQuery(std::string_view project, std::string_view dataset,
                           std::string_view table);

// Quotes a resolved table name as project.dataset.table. Domain-scoped project IDs may
// contain dots; only the last two dots delimit the dataset and table in that case.
std::string QuoteIdentifierPath(std::string_view path);

std::string ToHex(std::string_view value);

// `parts` with `separator` between each two.
std::string Join(const std::vector<std::string>& parts, std::string_view separator);

}  // namespace bigquery_emulator_duckdb
