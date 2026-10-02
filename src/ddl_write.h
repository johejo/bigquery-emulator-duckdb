#pragma once

#include <optional>
#include <string>
#include <vector>

#include "src/translator.h"

namespace bigquery_emulator_duckdb {

// What a DDL statement runs besides itself, in the same transaction: the statements that
// record its BigQuery metadata, and a query that returns a row when the statement and its
// metadata must both be skipped, which is how IF NOT EXISTS and IF EXISTS keep what is there.
struct DdlWrite {
  std::vector<std::string> metadata_statements;
  std::string skip_query;
};

// What CREATE TABLE of `definition` runs besides itself.
DdlWrite CreateTableWrite(const TableDefinition& definition);

// What CREATE VIEW of `view` runs besides itself.
DdlWrite CreateViewWrite(const ViewDefinition& view);

// What a translated statement runs besides itself, if it records metadata.
std::optional<DdlWrite> MetadataWrite(const TranslatedStatement& statement);

}  // namespace bigquery_emulator_duckdb
