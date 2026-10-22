#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/types/span.h"
#include "googlesql/public/analyzer_options.h"
#include "googlesql/public/simple_catalog_util.h"
#include "src/catalog.h"
#include "src/language_options.h"
#include "src/table_path.h"
#include "src/table_source.h"

namespace bigquery_emulator_duckdb {
namespace {
class GraphCatalog : public googlesql::SimpleCatalog {
 public:
  GraphCatalog(googlesql::Catalog& parent, googlesql::TypeFactory* factory)
      : SimpleCatalog("graph", factory), parent_(parent) {}
  absl::Status FindTable(const absl::Span<const std::string>& path, const googlesql::Table** table,
                         const FindOptions& options) override {
    return parent_.FindTable(path, table, options);
  }

 private:
  googlesql::Catalog& parent_;
};
}  // namespace

absl::Status BigQueryCatalog::FindPropertyGraph(absl::Span<const std::string> path,
                                                const googlesql::PropertyGraph*& graph,
                                                const FindOptions& options) {
  const auto parts = NormalizeTablePath(path, default_project_, default_dataset_, &source_);
  if (parts.size() != 3) {
    return absl::NotFoundError("Property graph name requires a dataset");
  }
  auto found = graphs_.find(parts);
  if (found == graphs_.end()) {
    const auto ddl = source_.FindPropertyGraph(
        {.project_id = parts.at(0), .dataset_id = parts.at(1), .table_id = parts.at(2)});
    if (!ddl) {
      return absl::NotFoundError("Property graph not found");
    }
    auto catalog = std::make_unique<GraphCatalog>(*this, type_factory_);
    const googlesql::AnalyzerOptions analyzer_options(GoogleSqlLanguageOptions());
    const auto status = googlesql::AddPropertyGraphFromCreatePropertyGraphStmt(
        *ddl, analyzer_options, graph_artifacts_, *catalog);
    if (!status.ok()) {
      return status;
    }
    found = graphs_.emplace(parts, std::move(catalog)).first;
  }
  return found->second->FindPropertyGraph({parts.back()}, graph, options);
}
}  // namespace bigquery_emulator_duckdb
