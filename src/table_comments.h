#pragma once

#include <optional>
#include <string>
#include <vector>

#include "nlohmann/json_fwd.hpp"
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
};

// The metadata a table or view comment records, or none for a comment of someone else's.
TableMetadata CommentMetadata(const nlohmann::json& comment);

// The statement that records `metadata` on the DuckDB table `table`, whose schema is `schema`.
// Throws ApiError::Invalid for metadata BigQuery rejects.
std::string TableCommentStatement(const TableReference& table, const TableMetadata& metadata,
                                  const std::vector<FieldSchema>& schema);

// The statement that records `view` on the DuckDB view `table`. Throws ApiError::Invalid for
// metadata a view cannot have.
std::string ViewCommentStatement(const TableReference& table, const ViewMetadata& view);

// Parses a view comment, or returns nothing for a view the emulator did not create, which has
// no comment or a comment of its own.
std::optional<ViewMetadata> ParseViewMetadata(const nlohmann::json& comment);

}  // namespace bigquery_emulator_duckdb
