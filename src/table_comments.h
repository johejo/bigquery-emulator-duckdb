#pragma once

#include <optional>
#include <string>
#include <vector>

#include "src/field_schema.h"
#include "src/references.h"
#include "src/table_metadata.h"

namespace bigquery_emulator_duckdb {

// The emulator records a table's TableMetadata as JSON in the comment of its DuckDB table, and a
// view's in the comment of its DuckDB view, next to the GoogleSQL query and its schema. Like a
// column comment, it lives and dies with the table, so CREATE OR REPLACE starts it afresh.
struct ViewMetadata {
  std::string query;
  std::vector<FieldSchema> schema;
  TableMetadata metadata;
  bool materialized = false;
  std::optional<MaterializedViewSource> materialized_source = {};
};

// The metadata a table or view comment records, or none for a comment of someone else's.
TableMetadata CommentMetadata(const std::optional<std::string>& comment);

// The statement that records `metadata` on the DuckDB table `table`, whose schema is `schema`,
// with its primary key spelled as `schema` spells the columns. Throws ApiError::Invalid for
// metadata BigQuery rejects.
std::string TableCommentStatement(const TableReference& table, TableMetadata metadata,
                                  const std::vector<FieldSchema>& schema);

// The statement that records `view` on the DuckDB view `table`. Throws ApiError::Invalid for
// metadata a view cannot have, and ApiError::Unsupported for a default rounding mode.
std::string ViewCommentStatement(const TableReference& table, const ViewMetadata& view);

// Parses a view comment, or returns nothing for a view the emulator did not create, which has
// no comment or a comment of its own.
std::optional<ViewMetadata> ParseViewMetadata(const std::optional<std::string>& comment);

}  // namespace bigquery_emulator_duckdb
