#include "src/translator/graph.h"

#include <algorithm>
#include <cstddef>
#include <format>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_set.h"
#include "googlesql/public/function.h"
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
#include "src/type_mapping.h"

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

std::string ElementAlias(const googlesql::ResolvedGraphElementScan& element) {
  return QuoteIdentifier("g" + std::to_string(element.column_list(0).column_id()));
}

std::string EndpointMatch(const googlesql::ResolvedGraphElementScan& node,
                          const googlesql::ResolvedGraphElementScan& edge,
                          const googlesql::GraphNodeTableReference& reference) {
  const auto* node_table = node.target_element_table_list(0);
  if (node_table != reference.GetReferencedNodeTable()) {
    return "FALSE";
  }
  const auto* edge_table = edge.target_element_table_list(0)->GetTable();
  std::vector<std::string> keys;
  keys.reserve(reference.GetEdgeTableColumns().size());
  for (size_t i = 0; i < reference.GetEdgeTableColumns().size(); ++i) {
    keys.push_back(std::format(
        "{}.{} = {}.{}", ElementAlias(edge),
        QuoteIdentifier(edge_table->GetColumn(reference.GetEdgeTableColumns().at(i))->Name()),
        ElementAlias(node),
        QuoteIdentifier(
            node_table->GetTable()->GetColumn(reference.GetNodeTableColumns().at(i))->Name())));
  }
  return "(" + Join(keys, " AND ") + ")";
}

// Resolution joins occurrences of a pattern variable using equality between distinct columns
// with the same name. Compare their identities; user-written graph comparisons still go
// through Expression and remain unsupported.
std::optional<std::string> GraphFilter(const googlesql::ResolvedExpr& expr, const Scope& scope,
                                       const Columns& columns) {
  if (expr.Is<googlesql::ResolvedFunctionCall>()) {
    const auto& call = *expr.GetAs<googlesql::ResolvedFunctionCall>();
    const auto name = call.function()->Name();
    if (name == "$and") {
      std::vector<std::string> terms;
      for (const auto& argument : call.argument_list()) {
        const auto term = GraphFilter(*argument, scope, columns);
        if (!term) {
          return std::nullopt;
        }
        terms.push_back(*term);
      }
      return "(" + Join(terms, " AND ") + ")";
    }
    if (name == "$equal" && call.argument_list_size() == 2 &&
        call.argument_list(0)->Is<googlesql::ResolvedColumnRef>() &&
        call.argument_list(1)->Is<googlesql::ResolvedColumnRef>()) {
      const auto& left = call.argument_list(0)->GetAs<googlesql::ResolvedColumnRef>()->column();
      const auto& right = call.argument_list(1)->GetAs<googlesql::ResolvedColumnRef>()->column();
      if (left.type()->IsGraphElement() && right.type()->IsGraphElement() &&
          left.column_id() != right.column_id() &&
          ToLowerAscii(left.name()) == ToLowerAscii(right.name())) {
        const auto lhs = Expression(*call.argument_list(0), scope, columns);
        const auto rhs = Expression(*call.argument_list(1), scope, columns);
        if (!lhs || !rhs) {
          return std::nullopt;
        }
        return "(struct_extract(" + *lhs + ", '$id') = struct_extract(" + *rhs + ", '$id'))";
      }
    }
  }
  return Expression(expr, scope, columns);
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

namespace {

std::optional<Relation> GraphPath(const googlesql::ResolvedGraphPathScan& path,
                                  const Scope& scope) {
  const auto mode = path.path_mode() == nullptr ? googlesql::ResolvedGraphPathMode::WALK
                                                : path.path_mode()->path_mode();
  if (path.quantifier() != nullptr || path.search_prefix() != nullptr ||
      path.path_cost() != nullptr || !path.hint_list().empty() || !path.path_hint_list().empty() ||
      (mode != googlesql::ResolvedGraphPathMode::WALK &&
       mode != googlesql::ResolvedGraphPathMode::PATH_MODE_UNSPECIFIED &&
       mode != googlesql::ResolvedGraphPathMode::TRAIL &&
       mode != googlesql::ResolvedGraphPathMode::ACYCLIC)) {
    return Unsupported(scope, "quantified paths, search prefixes or path modes");
  }
  Relation result;
  std::vector<std::string> outputs;
  std::vector<std::string> projections;
  std::vector<std::string> filters;
  std::vector<std::string> path_nodes;
  std::vector<std::string> path_edges;
  std::vector<std::string> mode_elements;
  std::vector<std::string> tables;
  std::vector<std::string> connections;
  std::vector<const googlesql::ResolvedGraphElementScan*> elements;
  for (const auto& part : path.input_scan_list()) {
    if (!part->Is<googlesql::ResolvedGraphElementScan>()) {
      return Unsupported(scope, "nested graph paths");
    }
    const auto& element = *part->GetAs<googlesql::ResolvedGraphElementScan>();
    if (element.target_element_table_list_size() != 1 || !element.hint_list().empty() ||
        element.column_list_size() != 1) {
      return Unsupported(scope, "graph patterns without a single table and label");
    }
    const auto& column = element.column_list(0);
    const std::string variable = ElementAlias(element);
    if (element.Is<googlesql::ResolvedGraphEdgeScan>()) {
      const auto& edge = *element.GetAs<googlesql::ResolvedGraphEdgeScan>();
      if (!edge.lhs_hint_list().empty() || !edge.rhs_hint_list().empty() ||
          edge.cost_expr() != nullptr) {
        return Unsupported(scope, "graph edge hints or costs");
      }
    }
    const auto* element_table = element.target_element_table_list(0);
    tables.push_back(QuoteIdentifierPath(element_table->GetTable()->FullName()) + " AS " +
                     variable);
    elements.push_back(&element);
    std::vector<std::string> keys;
    for (size_t i = 0; i < element_table->GetKeyColumns().size(); ++i) {
      const auto output =
          QuoteIdentifier("key_" + std::to_string(column.column_id()) + "_" + std::to_string(i));
      // A key need not be a graph property. Read the underlying column without exposing it
      // as a public property.
      const auto* key = element_table->GetTable()->GetColumn(element_table->GetKeyColumns().at(i));
      outputs.push_back(std::format("CAST({}.{} AS VARCHAR) AS {}", variable,
                                    QuoteIdentifier(key->Name()), output));
      keys.push_back("q." + output);
    }
    const auto identity =
        "CAST(to_json(struct_pack(\"$table\" := " + QuoteLiteral(element_table->FullName()) +
        ", \"$keys\" := [" + Join(keys, ", ") + "])) AS VARCHAR)";
    absl::flat_hash_set<const googlesql::GraphElementLabel*> labels;
    if (!element_table->GetLabels(labels).ok() || labels.size() != 1) {
      return Unsupported(scope, "graph element tables without one static label");
    }
    const auto label = (*labels.begin())->Name();
    std::vector<std::string> fields;
    for (const auto& property : column.type()->AsGraphElement()->property_types()) {
      const std::string name = QuoteIdentifier(property.name);
      const std::string output =
          QuoteIdentifier("g" + std::to_string(column.column_id()) + "_" + property.name);
      const googlesql::GraphPropertyDefinition* definition = nullptr;
      if (!element_table->FindPropertyDefinitionByName(property.name, definition).ok()) {
        return Unsupported(scope, "graph property definition");
      }
      const auto value = definition->GetValueExpression();
      if (!value.ok() || !(*value)->Is<googlesql::ResolvedCatalogColumnRef>()) {
        return Unsupported(scope, "graph properties other than columns");
      }
      const auto* source = (*value)->GetAs<googlesql::ResolvedCatalogColumnRef>()->column();
      outputs.push_back(
          std::format("{}.{} AS {}", variable, QuoteIdentifier(source->Name()), output));
      fields.push_back(std::format("{} := q.{}", name, output));
    }
    if (fields.empty()) {
      // DuckDB structs need a field even for a graph element with no properties.
      fields.emplace_back("\"$empty\" := TRUE");
    }
    const auto pack = [&](const std::vector<std::string>& properties) {
      return "struct_pack(\"$id\" := " + identity + ", \"$labels\" := [" + QuoteLiteral(label) +
             "], \"$properties\" := struct_pack(" + Join(properties, ", ") + "))";
    };
    projections.push_back(pack(fields) + " AS " + ColumnName(column.column_id()));
    const bool is_edge = element.Is<googlesql::ResolvedGraphEdgeScan>();
    if ((mode == googlesql::ResolvedGraphPathMode::TRAIL && is_edge) ||
        (mode == googlesql::ResolvedGraphPathMode::ACYCLIC && !is_edge)) {
      mode_elements.push_back("struct_extract(q." + ColumnName(column.column_id()) + ", '$id')");
    }
    if (path.path() != nullptr) {
      const auto* path_type = path.path()->column().type()->AsGraphPath();
      const auto* target = is_edge ? path_type->edge_type() : path_type->node_type();
      std::vector<std::string> path_properties;
      for (const auto& property : target->property_types()) {
        const auto property_type = DuckDbType(property.value_type);
        if (!property_type) {
          return Unsupported(scope, "graph path property type");
        }
        const auto& available = column.type()->AsGraphElement()->property_types();
        const bool present = std::ranges::any_of(available, [&](const auto& candidate) {
          return ToLowerAscii(candidate.name) == ToLowerAscii(property.name);
        });
        const auto output =
            QuoteIdentifier("g" + std::to_string(column.column_id()) + "_" + property.name);
        path_properties.push_back(
            QuoteIdentifier(property.name) +
            " := " + (present ? "q." + output : "CAST(NULL AS " + *property_type + ")"));
      }
      if (path_properties.empty()) {
        path_properties.emplace_back("\"$empty\" := TRUE");
      }
      (is_edge ? path_edges : path_nodes).push_back(pack(path_properties));
    }
    result.columns.emplace(column.column_id(), "q." + ColumnName(column.column_id()));
  }
  if (path.path() != nullptr) {
    const auto& column = path.path()->column();
    const auto* path_type = column.type()->AsGraphPath();
    const auto node_type = DuckDbType(path_type->node_type());
    const auto edge_type = DuckDbType(path_type->edge_type());
    if (!node_type || !edge_type) {
      return Unsupported(scope, "graph path element type");
    }
    projections.push_back("struct_pack(\"$nodes\" := CAST([" + Join(path_nodes, ", ") + "] AS " +
                          *node_type + "[]), \"$edges\" := CAST([" + Join(path_edges, ", ") +
                          "] AS " + *edge_type + "[])) AS " + ColumnName(column.column_id()));
    result.columns.emplace(column.column_id(), "q." + ColumnName(column.column_id()));
  }
  if (outputs.empty()) {
    outputs.emplace_back("1 AS _unit");
  }
  for (size_t i = 1; i + 1 < elements.size(); i += 2) {
    const auto& edge = *elements.at(i)->GetAs<googlesql::ResolvedGraphEdgeScan>();
    const auto* table = edge.target_element_table_list(0)->AsEdgeTable();
    const auto direction = [&](bool reverse) {
      return "(" +
             EndpointMatch(*elements.at(i - 1), edge,
                           *(reverse ? table->GetDestNodeTable() : table->GetSourceNodeTable())) +
             " AND " +
             EndpointMatch(*elements.at(i + 1), edge,
                           *(reverse ? table->GetSourceNodeTable() : table->GetDestNodeTable())) +
             ")";
    };
    // Match both orientations against the original edge row. OR emits a self-loop once
    // and preserves parallel edges, composite endpoint keys and edge properties.
    connections.push_back(
        edge.orientation() == googlesql::ResolvedGraphEdgeScan::ANY
            ? "(" + direction(false) + " OR " + direction(true) + ")"
            : direction(edge.orientation() == googlesql::ResolvedGraphEdgeScan::LEFT));
  }
  const auto rows = "SELECT " + Join(outputs, ", ") + " FROM " + Join(tables, " CROSS JOIN ") +
                    (connections.empty() ? "" : " WHERE " + Join(connections, " AND "));
  result.sql = "SELECT " + Join(projections, ", ") + " FROM (" + rows + ") AS q";
  if (mode == googlesql::ResolvedGraphPathMode::TRAIL ||
      mode == googlesql::ResolvedGraphPathMode::ACYCLIC) {
    filters.push_back("list_unique([" + Join(mode_elements, ", ") +
                      "]) = " + std::to_string(mode_elements.size()));
  }
  for (const auto& part : path.input_scan_list()) {
    const auto* filter = part->GetAs<googlesql::ResolvedGraphElementScan>()->filter_expr();
    if (filter != nullptr) {
      const auto expression = GraphFilter(*filter, scope, result.columns);
      if (!expression) {
        return std::nullopt;
      }
      filters.push_back(*expression);
    }
  }
  if (path.filter_expr() != nullptr) {
    const auto expression = GraphFilter(*path.filter_expr(), scope, result.columns);
    if (!expression) {
      return std::nullopt;
    }
    filters.push_back(*expression);
  }
  if (!filters.empty()) {
    result.sql = "SELECT q.*" + result.From() + " WHERE " + Join(filters, " AND ");
  }
  return result;
}

}  // namespace

std::optional<Relation> GraphScan(const googlesql::ResolvedGraphScan& scan, const Scope& scope) {
  if (scope.graph_name.empty() || scan.optional() ||
      (scan.input_scan() != nullptr &&
       !scan.input_scan()->Is<googlesql::ResolvedSingleRowScan>()) ||
      scan.input_scan_list().empty()) {
    return Unsupported(scope, "optional or chained graph patterns");
  }
  Relation result;
  std::vector<std::string> paths;
  std::vector<std::string> projections;
  for (const auto& path : scan.input_scan_list()) {
    const auto input = GraphPath(*path, scope);
    if (!input) {
      return std::nullopt;
    }
    const auto alias = QuoteIdentifier("p" + std::to_string(paths.size()));
    paths.push_back("(" + input->sql + ") AS " + alias);
    for (const auto& [id, unused] : input->columns) {
      projections.push_back(alias + "." + ColumnName(id) + " AS " + ColumnName(id));
      result.columns.emplace(id, "q." + ColumnName(id));
    }
  }
  result.sql = "SELECT " + Join(projections, ", ") + " FROM " + Join(paths, " CROSS JOIN ");
  if (scan.filter_expr() != nullptr) {
    const auto filter = GraphFilter(*scan.filter_expr(), scope, result.columns);
    if (!filter) {
      return std::nullopt;
    }
    result.sql = "SELECT q.*" + result.From() + " WHERE " + *filter;
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
