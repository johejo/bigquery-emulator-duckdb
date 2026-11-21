#pragma once

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_set.h"
#include "absl/status/status.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "googlesql/public/property_graph.h"

namespace googlesql {
class SimpleCatalog;
class TableValuedFunction;
class TypeFactory;
}  // namespace googlesql

namespace bigquery_emulator_duckdb {
class TableSource;

namespace analyzer_functions {

// A property graph that also keeps the order in which its definition lists its element tables,
// which GoogleSQL's graphs do not and GRAPH_EXPAND joins related tables in. `graph` must outlive
// it.
class OrderedPropertyGraph : public googlesql::PropertyGraph {
 public:
  // `order` names the node tables then the edge tables, each in definition order.
  OrderedPropertyGraph(const googlesql::PropertyGraph& graph, std::vector<std::string> order)
      : graph_(graph), order_(std::move(order)) {}

  [[nodiscard]] const std::vector<std::string>& element_table_order() const { return order_; }

  [[nodiscard]] std::string Name() const override { return graph_.Name(); }
  [[nodiscard]] absl::Span<const std::string> NamePath() const override {
    return graph_.NamePath();
  }
  [[nodiscard]] std::string FullName() const override { return graph_.FullName(); }
  absl::Status FindLabelByName(absl::string_view name,
                               const googlesql::GraphElementLabel*& label) const override {
    return graph_.FindLabelByName(name, label);
  }
  absl::Status FindPropertyDeclarationByName(
      absl::string_view name,
      const googlesql::GraphPropertyDeclaration*& property_declaration) const override {
    return graph_.FindPropertyDeclarationByName(name, property_declaration);
  }
  absl::Status FindElementTableByName(
      absl::string_view name, const googlesql::GraphElementTable*& element_table) const override {
    return graph_.FindElementTableByName(name, element_table);
  }
  absl::Status GetNodeTables(
      absl::flat_hash_set<const googlesql::GraphNodeTable*>& output) const override {
    return graph_.GetNodeTables(output);
  }
  absl::Status GetEdgeTables(
      absl::flat_hash_set<const googlesql::GraphEdgeTable*>& output) const override {
    return graph_.GetEdgeTables(output);
  }
  absl::Status GetLabels(
      absl::flat_hash_set<const googlesql::GraphElementLabel*>& output) const override {
    return graph_.GetLabels(output);
  }
  absl::Status GetPropertyDeclarations(
      absl::flat_hash_set<const googlesql::GraphPropertyDeclaration*>& output) const override {
    return graph_.GetPropertyDeclarations(output);
  }

 private:
  const googlesql::PropertyGraph& graph_;
  std::vector<std::string> order_;
};

// Supplements GoogleSQL's built-ins with BigQuery declarations for analysis, not
// translation rules or executable implementations. Types belong to the catalog's factory.
void AddBigQueryFunctionDeclarations(googlesql::SimpleCatalog& catalog);

// The caller owns the TVF. The source and type factory must outlive it; its result
// schema is described by the external database during statement analysis.
std::unique_ptr<googlesql::TableValuedFunction> MakeExternalQueryFunction(
    TableSource& source, std::string default_project, googlesql::TypeFactory* type_factory);

}  // namespace analyzer_functions
}  // namespace bigquery_emulator_duckdb
