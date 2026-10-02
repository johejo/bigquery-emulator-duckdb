#pragma once

#include <string>
#include <vector>

#include "nlohmann/json_fwd.hpp"
#include "src/field_schema.h"
#include "src/references.h"

namespace bigquery_emulator_duckdb {

// The emulator keeps each column's BigQuery TableFieldSchema in the comment of its DuckDB column.
// DuckDB's types cannot tell JSON or GEOGRAPHY from STRING, REQUIRED from NULLABLE or NUMERIC
// from BIGNUMERIC, and have no place for a description. A comment lives and dies with its
// column, so dropping or replacing a table or adding a column never leaves it stale.

// The statements that record `schema` on the columns of `table`, which must already exist.
std::vector<std::string> ColumnCommentStatements(const TableReference& table,
                                                 const std::vector<FieldSchema>& schema);

// A query for the comments of the columns of `table`, one row per column in column order.
std::string ColumnCommentsQuery(const TableReference& table);

// Replaces each field of `derived`, the schema read back from DuckDB's types, with the field its
// column's comment records. A column without one, such as one added by a DuckDB statement the
// emulator did not write, or one whose comment someone else wrote, keeps its derived field.
std::vector<FieldSchema> ApplyColumnComments(std::vector<FieldSchema> derived,
                                             const std::vector<nlohmann::json>& comments);

}  // namespace bigquery_emulator_duckdb
