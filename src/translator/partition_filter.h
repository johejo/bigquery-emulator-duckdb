#pragma once

namespace googlesql {
class ResolvedStatement;
}

namespace bigquery_emulator_duckdb::translator {

// Enforces required partition filters on reads, after views have been inlined.
void CheckPartitionFilters(const googlesql::ResolvedStatement& statement);

}  // namespace bigquery_emulator_duckdb::translator
