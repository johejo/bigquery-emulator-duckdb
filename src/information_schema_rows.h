#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "src/references.h"

namespace bigquery_emulator_duckdb::information_schema {

// A cell as a DuckDB literal; an absent value represents SQL NULL.
using Cell = std::optional<std::string>;
using Row = std::vector<Cell>;

inline const Cell kNull;
inline constexpr const char* kEmptyArray = "[]";

Cell String(const std::string& value);
Cell Int(int64_t value);
// A time in milliseconds since the Unix epoch as INFORMATION_SCHEMA spells it.
std::string FormatMillis(int64_t millis);
// An unknown creation time, recorded as zero, is NULL.
Cell CreationTime(int64_t millis);
// An unset metadata string is NULL.
Cell OptionalString(const std::string& value);
// The project, dataset and, for a table, its name, as row cells.
Row Identity(const DatasetReference& dataset);
Row Identity(const DatasetReference& dataset, const std::string& table);
// `identity` followed by `cells`.
Row Concat(Row identity, const Row& cells);

}  // namespace bigquery_emulator_duckdb::information_schema
