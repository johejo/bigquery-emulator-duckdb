#include <algorithm>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "googlesql/public/type.h"
#include "googlesql/resolved_ast/resolved_ast.h"
#include "src/catalog.h"
#include "src/duckdb_sql.h"
#include "src/references.h"
#include "src/translated_statement.h"
#include "src/translator/context.h"
#include "src/translator/ddl.h"
#include "src/translator/ddl_names.h"
#include "src/translator/ddl_options.h"
#include "src/translator/scan.h"
#include "src/type_mapping.h"

namespace bigquery_emulator_duckdb::translator {
namespace {

// Whether `query` reads a temporary table, which a view could no longer read once the
// multi-statement query that created it ends.
bool ReadsTemporaryTable(const googlesql::ResolvedScan& query, const Scope& scope) {
  const TemporaryTables* temporary = scope.context.defaults.temporary;
  if (temporary == nullptr) {
    return false;
  }
  std::vector<const googlesql::ResolvedNode*> scans;
  query.GetDescendantsWithKinds({googlesql::RESOLVED_TABLE_SCAN}, &scans);
  return std::ranges::any_of(scans, [&](const googlesql::ResolvedNode* scan) {
    return scan->GetAs<googlesql::ResolvedTableScan>()->table()->FullName().starts_with(
        temporary->project + ".");
  });
}

}  // namespace

std::optional<std::string> CreateView(const googlesql::ResolvedCreateViewStmt& create,
                                      const Scope& scope) {
  if (create.create_scope() != googlesql::ResolvedCreateStatement::CREATE_DEFAULT_SCOPE ||
      create.recursive() || create.is_value_table()) {
    return Unsupported(scope, "temporary, recursive or value-table views");
  }
  if (ReadsTemporaryTable(*create.query(), scope)) {
    return Unsupported(scope, "views that read temporary tables");
  }
  const auto path = TargetTable(create.name_path(), scope, true);
  const std::optional<TableReference> target = scope.context.ddl_target_table;
  const auto relation = Scan(*create.query(), scope);
  auto metadata = OptionsMetadata(create.option_list(), "CREATE VIEW", scope);
  if (!path || !target || !relation || !metadata) {
    return std::nullopt;
  }
  ViewDefinition view{
      .table = *target,
      .query = create.sql(),
      .metadata = *std::move(metadata),
      .if_not_exists =
          (create.create_mode() == googlesql::ResolvedCreateStatement::CREATE_IF_NOT_EXISTS),
  };
  std::vector<std::string> projections;
  for (const auto& output : create.output_column_list()) {
    const auto column = relation->columns.find(output->column().column_id());
    if (HasInterval(output->column().type())) {
      return Unsupported(scope, "stored INTERVAL columns");
    }
    const auto type = DuckDbType(output->column().type());
    if (column == relation->columns.end() || !type) {
      return std::nullopt;
    }
    auto field = BigQueryFieldSchema(output->name(), output->column().type());
    if (!field.ok()) {
      return Unsupported(scope, field.status().message());
    }
    view.schema.push_back(*std::move(field));
    // Preserve GoogleSQL result types when the view is later read through the catalog.
    projections.push_back("CAST(" + column->second + " AS " + *type + ") AS " +
                          QuoteIdentifier(output->name()));
  }
  std::string head = "CREATE ";
  if (create.create_mode() == googlesql::ResolvedCreateStatement::CREATE_OR_REPLACE) {
    head += "OR REPLACE ";
  }
  head += "VIEW ";
  if (create.create_mode() == googlesql::ResolvedCreateStatement::CREATE_IF_NOT_EXISTS) {
    head += "IF NOT EXISTS ";
  }
  scope.context.view = std::move(view);
  return head + *path + " AS SELECT " + Join(projections, ", ") + relation->From() +
         relation->Order();
}

}  // namespace bigquery_emulator_duckdb::translator
