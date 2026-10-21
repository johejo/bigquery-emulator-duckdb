#pragma once

#include <optional>
#include <string>
#include <vector>

#include "src/field_schema.h"
#include "src/location.h"
#include "src/references.h"
#include "src/table_metadata.h"
#include "src/translated_statement.h"

namespace googlesql {
class Value;
}  // namespace googlesql

namespace bigquery_emulator_duckdb {

// What a DDL statement runs besides itself, in the same transaction: the statements that
// record its BigQuery metadata or copy rows into the table it creates, and a query that returns a
// row when the statement and its metadata must both be skipped, which is how IF NOT EXISTS and IF
// EXISTS keep what is there.
struct DdlWrite {
  std::vector<std::string> metadata_statements;
  std::string skip_query = {};
};

// Checks a literal column default against its REQUIRED mode and type parameters. A non-literal
// default has no `literal_value` and is rejected for parameterized fields.
void ValidateColumnDefault(const FieldSchema& field,
                           const std::optional<googlesql::Value>& literal_value);

// What CREATE SCHEMA and datasets.insert record for a new dataset: its metadata and the time it
// was created. A dataset whose metadata names no location is created in `location`, the job's.
DdlWrite CreateDatasetWrite(const DatasetDefinition& definition,
                            const std::string& location = kDefaultLocation);

// The statements that record `metadata` for `dataset`, replacing what was recorded before.
// Throws ApiError::Invalid for labels BigQuery rejects.
std::vector<std::string> DatasetMetadataStatements(const DatasetReference& dataset,
                                                   const DatasetMetadata& metadata);

// What DROP SCHEMA of `dataset` runs besides itself: it forgets the dataset's metadata.
DdlWrite DropDatasetWrite(const DatasetReference& dataset);

// What table creation runs besides CREATE TABLE: column metadata and defaults, table metadata
// with a fresh creation time, and any rows to copy. DDL, REST and destination jobs execute these
// statements in the same transaction as creation; their callers decide what to inherit or replace.
// A table that sets no expiration or default rounding mode takes the defaults of `dataset`, the
// dataset it is created in, unless it is temporary, and its NUMERIC and BIGNUMERIC fields without
// a rounding mode take the table's default unless `definition` keeps their modes. A COPY or CLONE
// keeps the source's default rounding mode even when it is unset. Throws
// ApiError::Unsupported for CREATE TABLE AS SELECT into a column the dataset's default makes round
// to even, since its values are rounded as translated.
DdlWrite CreateTableWrite(const TableDefinition& definition, const DatasetMetadata& dataset);

// What CREATE VIEW of `view` runs besides itself. A view that sets no expiration takes the
// default of `dataset`, as a table does.
DdlWrite CreateViewWrite(const ViewDefinition& view, const DatasetMetadata& dataset);

// The statements that apply `alteration` to its table, whose BigQuery schema and metadata are
// `schema` and `metadata` when it runs, or none when its actions all have nothing to do. An added
// column takes the table's default rounding mode. Throws ApiError::Invalid for an action BigQuery
// rejects.
std::vector<std::string> AlterTableStatements(const TableAlteration& alteration,
                                              std::vector<FieldSchema> schema,
                                              TableMetadata metadata);

// The statements that apply `alteration` to its dataset, whose metadata is `metadata` when it
// runs. Throws ApiError::Invalid for labels BigQuery rejects.
std::vector<std::string> AlterDatasetStatements(const DatasetAlteration& alteration,
                                                DatasetMetadata metadata);

// What a translated statement runs besides itself, if it records metadata. `dataset` is the
// metadata of the dataset a table or view it creates belongs to, and `location` that of the job,
// where CREATE SCHEMA creates a dataset that names no location.
std::optional<DdlWrite> MetadataWrite(const TranslatedStatement& statement,
                                      const DatasetMetadata& dataset, const std::string& location);

}  // namespace bigquery_emulator_duckdb
