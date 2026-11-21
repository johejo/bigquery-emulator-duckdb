#pragma once

#include <optional>
#include <string>
#include <vector>

namespace googlesql {
// Expose the analyzed body without making layout consumers parse the resolved AST.
class ResolvedQueryStmt;
// Keep the TVF resolver and signature declarations out of the layout interface.
class TVFSignature;
}  // namespace googlesql

namespace bigquery_emulator_duckdb::analyzer_functions {

// A visible output column or nested ROW field. `body_index` names a scalar output of the
// execution query, or the LEFT JOIN presence marker for a ROW.
struct GraphExpandColumn {
  std::string name;
  int body_index;
  bool is_row = false;
  std::vector<GraphExpandColumn> fields;
};

struct GraphExpandBody {
  const googlesql::ResolvedQueryStmt& query;
  const std::vector<GraphExpandColumn>& columns;
};

// The SQL body produces flat intermediate columns; the analyzer separately resolves a schema
// with ROW columns, and rewrites references to them into STRUCT field projections.
std::optional<GraphExpandBody> GetGraphExpandBody(const googlesql::TVFSignature& signature);

}  // namespace bigquery_emulator_duckdb::analyzer_functions
