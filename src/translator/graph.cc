#include "src/translator/graph.h"

#include <algorithm>
#include <format>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "googlesql/public/property_graph.h"
#include "googlesql/public/strings.h"
#include "googlesql/public/type.h"
#include "googlesql/resolved_ast/resolved_ast.h"
#include "src/catalog_table.h"
#include "src/duckdb_sql.h"
#include "src/references.h"
#include "src/table_path.h"
#include "src/translated_statement.h"
#include "src/translator/context.h"
#include "src/translator/expression.h"
#include "src/translator/scan.h"

namespace bigquery_emulator_duckdb::translator {
namespace {

bool PropertyType(const googlesql::Type* type) {
  return type->IsInt64() || type->IsString() || type->IsBool() || type->IsDouble();
}

std::optional<std::string> Key(const googlesql::ResolvedExpr& expr,
                               const googlesql::ResolvedTableScan& scan, bool native) {
  if (!expr.Is<googlesql::ResolvedColumnRef>()) {
    return std::nullopt;
  }
  const int id = expr.GetAs<googlesql::ResolvedColumnRef>()->column().column_id();
  for (int i = 0; i < scan.column_list_size(); ++i) {
    if (scan.column_list(i).column_id() == id) {
      const auto name = scan.table()->GetColumn(scan.column_index_list(i))->Name();
      return native ? QuoteIdentifier(name) : googlesql::ToIdentifierLiteral(name);
    }
  }
  return std::nullopt;
}

std::optional<std::string> Keys(
    const std::vector<std::unique_ptr<const googlesql::ResolvedExpr>>& keys,
    const googlesql::ResolvedTableScan& scan, bool native) {
  std::vector<std::string> names;
  for (const auto& key : keys) {
    const auto name = Key(*key, scan, native);
    if (!name || (!key->type()->IsInt64() && !key->type()->IsString()) ||
        key->type_annotation_map() != nullptr) {
      return std::nullopt;
    }
    names.push_back(*name);
  }
  return "(" + Join(names, ", ") + ")";
}

}  // namespace

std::optional<std::string> CreatePropertyGraph(
    const googlesql::ResolvedCreatePropertyGraphStmt& create, const Scope& scope) {
  if (create.create_scope() != googlesql::ResolvedCreateStatement::CREATE_DEFAULT_SCOPE ||
      create.create_mode() != googlesql::ResolvedCreateStatement::CREATE_DEFAULT ||
      !create.option_list().empty()) {
    return Unsupported(scope, "property graph creation modes or options");
  }
  if (std::ranges::any_of(create.label_list(),
                          [](const auto& label) { return !label->options_list().empty(); })) {
    return Unsupported(scope, "graph label options");
  }
  const auto parts =
      NormalizeTablePath(create.name_path(), scope.context.defaults.project,
                         scope.context.defaults.dataset, scope.context.defaults.source);
  if (parts.size() != 3) {
    return Unsupported(scope, "property graph name without a dataset");
  }
  const TableReference target{
      .project_id = parts.at(0),
      .dataset_id = parts.at(1),
      .table_id = parts.at(2),
  };
  std::vector<std::string> quoted_parts;
  std::ranges::transform(parts, std::back_inserter(quoted_parts),
                         [](const auto& part) { return googlesql::ToIdentifierLiteral(part); });
  const auto quoted_graph = Join(quoted_parts, ".");
  std::map<std::string, const googlesql::ResolvedGraphElementTable*> nodes;
  std::ranges::transform(create.node_table_list(), std::inserter(nodes, nodes.end()),
                         [](const auto& node) { return std::pair(node->alias(), node.get()); });
  std::vector<std::string> native_nodes;
  std::vector<std::string> native_edges;
  std::vector<std::string> sql_nodes;
  std::vector<std::string> sql_edges;
  for (const bool edge : {false, true}) {
    for (const auto& element : edge ? create.edge_table_list() : create.node_table_list()) {
      if (!element->input_scan()->Is<googlesql::ResolvedTableScan>() ||
          element->label_name_list_size() != 1 || element->dynamic_label() != nullptr ||
          element->dynamic_properties() != nullptr) {
        return Unsupported(
            scope, "graph element tables with views, multiple labels or dynamic properties");
      }
      const auto& table = *element->input_scan()->GetAs<googlesql::ResolvedTableScan>();
      if (table.for_system_time_expr() != nullptr || !table.hint_list().empty()) {
        return Unsupported(scope, "graph element table modifiers");
      }
      const auto* stored = dynamic_cast<const BigQueryTable*>(table.table());
      const auto description = stored == nullptr ? std::nullopt : stored->Describe();
      if (stored == nullptr || stored->NumericReadQuery() ||
          (description && description->metadata.require_partition_filter)) {
        return Unsupported(
            scope, "graph element tables with external storage or required partition filters");
      }
      const auto keys = Keys(element->key_list(), table, false);
      if (!keys || element->key_list().empty()) {
        return Unsupported(scope, "graph keys other than columns");
      }
      const auto alias = googlesql::ToIdentifierLiteral(element->alias());
      std::string sql = googlesql::ToIdentifierLiteral(table.table()->FullName()) + " AS " + alias +
                        " KEY " + *keys;
      std::string native = QuoteIdentifierPath(table.table()->FullName()) + " AS " +
                           QuoteIdentifier(element->alias());
      if (edge) {
        for (const bool source : {true, false}) {
          const auto* ref =
              source ? element->source_node_reference() : element->dest_node_reference();
          const auto found =
              ref == nullptr ? nodes.end() : nodes.find(ref->node_table_identifier());
          if (found == nodes.end() ||
              !found->second->input_scan()->Is<googlesql::ResolvedTableScan>()) {
            return Unsupported(scope, "graph edge endpoints");
          }
          const auto& node_table =
              *found->second->input_scan()->GetAs<googlesql::ResolvedTableScan>();
          const auto edge_keys = Keys(ref->edge_table_column_list(), table, false);
          const auto node_keys = Keys(ref->node_table_column_list(), node_table, false);
          const auto native_edge_keys = Keys(ref->edge_table_column_list(), table, true);
          const auto native_node_keys = Keys(ref->node_table_column_list(), node_table, true);
          if (!edge_keys || !node_keys || !native_edge_keys || !native_node_keys) {
            return Unsupported(scope, "graph endpoint keys other than columns");
          }
          const std::string side = source ? " SOURCE KEY " : " DESTINATION KEY ";
          sql +=
              std::format("{}{} REFERENCES {}{}", side, *edge_keys,
                          googlesql::ToIdentifierLiteral(ref->node_table_identifier()), *node_keys);
          native += std::format("{}{} REFERENCES {}{}", side, *native_edge_keys,
                                QuoteIdentifier(ref->node_table_identifier()), *native_node_keys);
        }
      }
      std::vector<std::string> properties;
      std::vector<std::string> native_properties;
      for (const auto& property : element->property_definition_list()) {
        const auto column = Key(*property->expr(), table, false);
        const auto native_column = Key(*property->expr(), table, true);
        if (!column || !native_column || !PropertyType(property->expr()->type()) ||
            !property->options_list().empty() || property->is_measure() ||
            property->expr()->type_annotation_map() != nullptr) {
          return Unsupported(scope,
                             "graph properties other than INT64, STRING, BOOL or FLOAT64 columns");
        }
        properties.push_back(*column + " AS " +
                             googlesql::ToIdentifierLiteral(property->property_declaration_name()));
        native_properties.push_back(*native_column + " AS " +
                                    QuoteIdentifier(property->property_declaration_name()));
      }
      const std::string label = element->label_name_list(0);
      sql += std::format(
          " LABEL {}{}", googlesql::ToIdentifierLiteral(label),
          properties.empty() ? " NO PROPERTIES" : " PROPERTIES (" + Join(properties, ", ") + ")");
      native +=
          std::format("{} LABEL {}",
                      properties.empty() ? " NO PROPERTIES"
                                         : " PROPERTIES (" + Join(native_properties, ", ") + ")",
                      QuoteIdentifier(label));
      (edge ? sql_edges : sql_nodes).push_back(sql);
      (edge ? native_edges : native_nodes).push_back(native);
    }
  }
  scope.context.statement.property_graph = target;
  scope.context.statement.property_graph_definition =
      "CREATE PROPERTY GRAPH " + quoted_graph + " NODE TABLES (" + Join(sql_nodes, ", ") + ")" +
      (sql_edges.empty() ? "" : " EDGE TABLES (" + Join(sql_edges, ", ") + ")");
  // Graphs have a private name in DuckPGQ because its registry is database-wide.
  return "CREATE OR REPLACE PROPERTY GRAPH " + DuckPGQName(parts) + " VERTEX TABLES (" +
         Join(native_nodes, ", ") + ")" +
         (native_edges.empty() ? "" : " EDGE TABLES (" + Join(native_edges, ", ") + ")");
}

std::optional<Relation> GraphScan(const googlesql::ResolvedGraphScan& scan, const Scope& scope) {
  if (scope.graph_name.empty() || scan.optional() ||
      (scan.input_scan() != nullptr &&
       !scan.input_scan()->Is<googlesql::ResolvedSingleRowScan>()) ||
      scan.input_scan_list_size() != 1) {
    return Unsupported(scope, "optional, chained or multiple graph patterns");
  }
  const auto& path = *scan.input_scan_list(0);
  if (path.quantifier() != nullptr || path.path() != nullptr || path.search_prefix() != nullptr ||
      path.path_cost() != nullptr || !path.hint_list().empty() || !path.path_hint_list().empty() ||
      (path.path_mode() != nullptr &&
       path.path_mode()->path_mode() != googlesql::ResolvedGraphPathMode::WALK &&
       path.path_mode()->path_mode() != googlesql::ResolvedGraphPathMode::PATH_MODE_UNSPECIFIED)) {
    return Unsupported(scope, "quantified paths, path values, search prefixes or path modes");
  }
  Relation result;
  std::vector<std::string> outputs;
  std::vector<std::string> projections;
  std::vector<std::string> filters;
  std::string pattern;
  for (const auto& part : path.input_scan_list()) {
    if (!part->Is<googlesql::ResolvedGraphElementScan>()) {
      return Unsupported(scope, "nested graph paths");
    }
    const auto& element = *part->GetAs<googlesql::ResolvedGraphElementScan>();
    if (element.target_element_table_list_size() != 1 || !element.hint_list().empty() ||
        !element.label_expr()->Is<googlesql::ResolvedGraphLabel>() ||
        element.column_list_size() != 1) {
      return Unsupported(scope, "graph patterns without a single table and label");
    }
    const auto& column = element.column_list(0);
    const std::string variable = QuoteIdentifier("g" + std::to_string(column.column_id()));
    const std::string body =
        variable + ":" +
        QuoteIdentifier(
            element.label_expr()->GetAs<googlesql::ResolvedGraphLabel>()->label()->Name());
    if (element.Is<googlesql::ResolvedGraphEdgeScan>()) {
      const auto& edge = *element.GetAs<googlesql::ResolvedGraphEdgeScan>();
      if (!edge.lhs_hint_list().empty() || !edge.rhs_hint_list().empty() ||
          edge.cost_expr() != nullptr ||
          edge.orientation() == googlesql::ResolvedGraphEdgeScan::ANY) {
        return Unsupported(scope, "undirected graph edges or edge hints");
      }
      pattern += edge.orientation() == googlesql::ResolvedGraphEdgeScan::LEFT
                     ? std::format("<-[{}]-", body)
                     : std::format("-[{}]->", body);
    } else {
      pattern += std::format("({})", body);
    }
    std::vector<std::string> fields;
    for (const auto& property : column.type()->AsGraphElement()->property_types()) {
      const std::string name = QuoteIdentifier(property.name);
      const std::string output =
          QuoteIdentifier("g" + std::to_string(column.column_id()) + "_" + property.name);
      outputs.push_back(std::format("{}.{} AS {}", variable, name, output));
      fields.push_back(std::format("{} := q.{}", name, output));
    }
    if (fields.empty()) {
      // DuckDB structs need a field even for a graph element with no properties.
      fields.emplace_back("_unit := 1");
    }
    projections.push_back("struct_pack(" + Join(fields, ", ") + ") AS " +
                          ColumnName(column.column_id()));
    result.columns.emplace(column.column_id(), "q." + ColumnName(column.column_id()));
  }
  if (outputs.empty()) {
    outputs.emplace_back("1 AS _unit");
  }
  result.sql = "SELECT " + Join(projections, ", ") + " FROM GRAPH_TABLE(" + scope.graph_name +
               " MATCH " + pattern + " COLUMNS (" + Join(outputs, ", ") + ")) AS q";
  for (const auto& part : path.input_scan_list()) {
    const auto* filter = part->GetAs<googlesql::ResolvedGraphElementScan>()->filter_expr();
    if (filter != nullptr) {
      const auto expression = Expression(*filter, scope, result.columns);
      if (!expression) {
        return std::nullopt;
      }
      filters.push_back(*expression);
    }
  }
  for (const auto* filter : {path.filter_expr(), scan.filter_expr()}) {
    if (filter != nullptr) {
      const auto expression = Expression(*filter, scope, result.columns);
      if (!expression) {
        return std::nullopt;
      }
      filters.push_back(*expression);
    }
  }
  if (!filters.empty()) {
    result.sql = "SELECT q.*" + result.From() + " WHERE " + Join(filters, " AND ");
  }
  return result;
}

std::optional<Relation> GraphTableScan(const googlesql::ResolvedGraphTableScan& scan,
                                       const Scope& scope) {
  if (!scan.shape_expr_list().empty()) {
    return Unsupported(scope, "GRAPH_TABLE COLUMNS; use RETURN");
  }
  scope.context.statement.reads_property_graph = true;
  Scope graph_scope = scope;
  const auto names = scan.property_graph()->NamePath();
  graph_scope.graph_name = DuckPGQName({names.begin(), names.end()});
  return Scan(*scan.input_scan(), graph_scope);
}
}  // namespace bigquery_emulator_duckdb::translator
