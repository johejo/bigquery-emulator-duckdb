#pragma once

#include <optional>
#include <string>
#include <vector>

#include "src/references.h"
#include "src/table_metadata.h"
#include "src/translator.h"

namespace bigquery_emulator_duckdb {

// What a DDL statement runs besides itself, in the same transaction: the statements that
// record its BigQuery metadata, and a query that returns a row when the statement and its
// metadata must both be skipped, which is how IF NOT EXISTS and IF EXISTS keep what is there.
struct DdlWrite {
  std::vector<std::string> metadata_statements;
  std::string skip_query = {};
};

// DuckDB cannot comment on a schema, so the emulator records each dataset's DatasetMetadata as
// JSON in this table of the project's `main` schema, which is not a dataset. A dataset the table
// has no row for has no metadata. Writes to it share the transaction that creates or drops the
// dataset's schema.
std::string DatasetMetadataTable(const std::string& project);

// The statements that record `metadata` for `dataset`, replacing what was recorded before.
// Throws ApiError::Invalid for labels BigQuery rejects.
std::vector<std::string> DatasetMetadataStatements(const DatasetReference& dataset,
                                                   const DatasetMetadata& metadata);

// What DROP SCHEMA of `dataset` runs besides itself: it forgets the dataset's metadata.
DdlWrite DropDatasetWrite(const DatasetReference& dataset);

// What CREATE TABLE of `definition` runs besides itself.
DdlWrite CreateTableWrite(const TableDefinition& definition);

// What CREATE VIEW of `view` runs besides itself.
DdlWrite CreateViewWrite(const ViewDefinition& view);

// What a translated statement runs besides itself, if it records metadata.
std::optional<DdlWrite> MetadataWrite(const TranslatedStatement& statement);

}  // namespace bigquery_emulator_duckdb
