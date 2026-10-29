#include "src/translator/graph.h"

#include <algorithm>
#include <cstddef>
#include <format>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
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

constexpr std::string_view kUnsupportedKeys =
    "graph keys of type FLOAT64, BIGNUMERIC, INTERVAL, RANGE, GEOGRAPHY, JSON, ARRAY or STRUCT, or "
    "other than columns";

// Identities compare keys as text, which distinguishes FLOAT64 values that compare equal, such
// as 0 and -0.
bool KeyType(const googlesql::Type* type) {
  return type->IsInt64() || type->IsString() || type->IsBytes() || type->IsBool() ||
         type->IsDate() || type->IsDatetime() || type->IsTime() || type->IsTimestamp() ||
         type->IsNumericType();
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
    if (!name || !KeyType(key->type()) || key->type_annotation_map() != nullptr) {
      return std::nullopt;
    }
    names.push_back(*name);
  }
  return "(" + Join(names, ", ") + ")";
}

std::string ElementAlias(const googlesql::ResolvedGraphElementScan& element) {
  return QuoteIdentifier("g" + std::to_string(element.column_list(0).column_id()));
}

std::string ElementIdentity(const googlesql::GraphElementTable& table,
                            const std::vector<std::string>& keys) {
  return "CAST(to_json(struct_pack(\"$table\" := " + QuoteLiteral(table.FullName()) +
         ", \"$keys\" := [" + Join(keys, ", ") + "])) AS VARCHAR)";
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

// Read identities from the matched node rows, including when an endpoint references
// columns other than the node's element key. Traversal direction does not change them.
std::string EndpointIdentity(const googlesql::ResolvedGraphEdgeScan& edge,
                             const googlesql::ResolvedGraphElementScan& left,
                             const googlesql::ResolvedGraphElementScan& right, bool source) {
  const auto identity = [](const googlesql::ResolvedGraphElementScan& node) {
    const auto* table = node.target_element_table_list(0);
    std::vector<std::string> keys;
    std::ranges::transform(table->GetKeyColumns(), std::back_inserter(keys), [&](int key) {
      return "CAST(" + ElementAlias(node) + "." +
             QuoteIdentifier(table->GetTable()->GetColumn(key)->Name()) + " AS VARCHAR)";
    });
    return ElementIdentity(*table, keys);
  };
  const auto lhs = identity(source ? left : right);
  const auto rhs = identity(source ? right : left);
  if (edge.orientation() == googlesql::ResolvedGraphEdgeScan::ANY) {
    const auto* table = edge.target_element_table_list(0)->AsEdgeTable();
    const auto forward = EndpointMatch(left, edge, *table->GetSourceNodeTable()) + " AND " +
                         EndpointMatch(right, edge, *table->GetDestNodeTable());
    return "CASE WHEN " + forward + " THEN " + lhs + " ELSE " + rhs + " END";
  }
  return edge.orientation() == googlesql::ResolvedGraphEdgeScan::LEFT ? rhs : lhs;
}

const googlesql::ResolvedColumnRef* GraphColumn(const googlesql::ResolvedExpr& expr) {
  const auto* value = &expr;
  // The resolver casts repeated variables to a common property set when their labels differ.
  while (value->Is<googlesql::ResolvedCast>() && value->type()->IsGraphElement()) {
    value = value->GetAs<googlesql::ResolvedCast>()->expr();
  }
  return value->Is<googlesql::ResolvedColumnRef>() ? value->GetAs<googlesql::ResolvedColumnRef>()
                                                   : nullptr;
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
        GraphColumn(*call.argument_list(0)) != nullptr &&
        GraphColumn(*call.argument_list(1)) != nullptr) {
      const auto* lhs_ref = GraphColumn(*call.argument_list(0));
      const auto* rhs_ref = GraphColumn(*call.argument_list(1));
      const auto& left = lhs_ref->column();
      const auto& right = rhs_ref->column();
      if (left.type()->IsGraphElement() && right.type()->IsGraphElement() &&
          left.column_id() != right.column_id() &&
          ToLowerAscii(left.name()) == ToLowerAscii(right.name())) {
        const auto lhs = Expression(*lhs_ref, scope, columns);
        const auto rhs = Expression(*rhs_ref, scope, columns);
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
      if (stored == nullptr || stored->ReadQuery() ||
          (description && description->metadata.require_partition_filter)) {
        return Unsupported(
            scope, "graph element tables with external storage or required partition filters");
      }
      const auto keys = Keys(element->key_list(), table, false);
      if (!keys || element->key_list().empty()) {
        return Unsupported(scope, kUnsupportedKeys);
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
            return Unsupported(scope, kUnsupportedKeys);
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
        if (!column || !native_column || !DuckDbType(property->expr()->type()) ||
            !property->options_list().empty() || property->is_measure() ||
            property->expr()->type_annotation_map() != nullptr) {
          return Unsupported(scope, "computed properties, measures or property options");
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

// A flat path also exposes private traversal columns used to compose and repeat paths.
std::optional<Relation> FlatGraphPath(
    const std::vector<const googlesql::ResolvedGraphElementScan*>& input,
    const googlesql::GraphPathType* path_type, const Scope& scope) {
  auto elements = input;
  std::vector<std::unique_ptr<googlesql::ResolvedGraphNodeScan>> inferred;
  for (size_t i = 0; i < elements.size(); ++i) {
    const auto& element = *elements.at(i);
    if (!element.Is<googlesql::ResolvedGraphNodeScan>() ||
        element.target_element_table_list_size() == 1) {
      continue;
    }
    if (!element.hint_list().empty()) {
      return Unsupported(scope, "graph node hints");
    }
    auto candidates = element.target_element_table_list();
    for (const int offset : {-1, 1}) {
      const auto adjacent = static_cast<int>(i) + offset;
      if (adjacent < 0 || std::cmp_greater_equal(adjacent, elements.size()) ||
          !elements.at(adjacent)->Is<googlesql::ResolvedGraphEdgeScan>()) {
        continue;
      }
      const auto& edge = *elements.at(adjacent)->GetAs<googlesql::ResolvedGraphEdgeScan>();
      if (edge.target_element_table_list_size() != 1) {
        continue;
      }
      const auto* table = edge.target_element_table_list(0)->AsEdgeTable();
      const auto* source = table->GetSourceNodeTable()->GetReferencedNodeTable();
      const auto* destination = table->GetDestNodeTable()->GetReferencedNodeTable();
      const auto* endpoint =
          (offset == 1) == (edge.orientation() != googlesql::ResolvedGraphEdgeScan::LEFT)
              ? source
              : destination;
      std::erase_if(candidates, [&](const auto* candidate) {
        return edge.orientation() == googlesql::ResolvedGraphEdgeScan::ANY
                   ? candidate != source && candidate != destination
                   : candidate != endpoint;
      });
    }
    if (candidates.size() != 1) {
      return Unsupported(scope, "graph patterns without a single table and label");
    }
    auto node = googlesql::MakeResolvedGraphNodeScan();
    node->set_column_list(element.column_list());
    node->set_target_element_table_list(candidates);
    // Keep the original filter: the inferred table only narrows impossible endpoints.
    elements.at(i) = node.get();
    inferred.push_back(std::move(node));
  }
  Relation result;
  std::vector<std::string> outputs;
  std::vector<std::string> projections;
  std::vector<std::string> filters;
  std::vector<std::string> path_nodes;
  std::vector<std::string> path_edges;
  std::vector<std::string> node_values;
  std::vector<std::string> edge_values;
  std::vector<std::string> tables;
  std::vector<std::string> connections;
  for (size_t index = 0; index < elements.size(); ++index) {
    const auto& element = *elements.at(index);
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
    const auto identity = ElementIdentity(*element_table, keys);
    std::string endpoints;
    if (element.Is<googlesql::ResolvedGraphEdgeScan>()) {
      const auto& edge = *element.GetAs<googlesql::ResolvedGraphEdgeScan>();
      const auto& left = *elements.at(index - 1);
      const auto& right = *elements.at(index + 1);
      for (const bool source : {true, false}) {
        const auto endpoint = EndpointIdentity(edge, left, right, source);
        const std::string field = source ? "$source_id" : "$destination_id";
        const auto output = QuoteIdentifier(field + "_" + std::to_string(column.column_id()));
        outputs.push_back(std::format("{} AS {}", endpoint, output));
        endpoints += ", " + QuoteIdentifier(field) + " := q." + output;
      }
    }
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
        const auto type =
            DuckDbType(property.value_type, nullptr, scope.context.timestamp_precision);
        if (!type) {
          return Unsupported(scope, "graph property type");
        }
        outputs.push_back("CAST(NULL AS " + *type + ") AS " + output);
        fields.push_back(std::format("{} := q.{}", name, output));
        continue;
      }
      const auto value = definition->GetValueExpression();
      if (!value.ok() || !(*value)->Is<googlesql::ResolvedCatalogColumnRef>()) {
        return Unsupported(scope, "graph properties other than columns");
      }
      const auto* source = (*value)->GetAs<googlesql::ResolvedCatalogColumnRef>()->column();
      outputs.push_back(
          std::format("{} AS {}",
                      StoredColumn(variable + "." + QuoteIdentifier(source->Name()),
                                   source->GetType(), scope.context.timestamp_precision),
                      output));
      fields.push_back(std::format("{} := q.{}", name, output));
    }
    if (fields.empty()) {
      // DuckDB structs need a field even for a graph element with no properties.
      fields.emplace_back("\"$empty\" := TRUE");
    }
    const auto pack = [&](const std::vector<std::string>& properties) {
      return std::format(
          "struct_pack(\"$id\" := {}{}, \"$labels\" := [{}], "
          "\"$properties\" := struct_pack({}))",
          identity, endpoints, QuoteLiteral(label), Join(properties, ", "));
    };
    projections.push_back(pack(fields) + " AS " + ColumnName(column.column_id()));
    const bool is_edge = element.Is<googlesql::ResolvedGraphEdgeScan>();
    (is_edge ? path_edges : path_nodes)
        .push_back("struct_extract(q." + ColumnName(column.column_id()) + ", '$id')");
    if (path_type != nullptr) {
      const auto* target = is_edge ? path_type->edge_type() : path_type->node_type();
      std::vector<std::string> path_properties;
      for (const auto& property : target->property_types()) {
        const auto property_type =
            DuckDbType(property.value_type, nullptr, scope.context.timestamp_precision);
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
      (is_edge ? edge_values : node_values).push_back(pack(path_properties));
    }
    result.columns.emplace(column.column_id(), "q." + ColumnName(column.column_id()));
  }
  if (path_type != nullptr) {
    const auto node_type =
        DuckDbType(path_type->node_type(), nullptr, scope.context.timestamp_precision);
    const auto edge_type =
        DuckDbType(path_type->edge_type(), nullptr, scope.context.timestamp_precision);
    if (!node_type || !edge_type) {
      return Unsupported(scope, "graph path element type");
    }
    projections.push_back("struct_pack(\"$nodes\" := CAST([" + Join(node_values, ", ") + "] AS " +
                          *node_type + "[]), \"$edges\" := CAST([" + Join(edge_values, ", ") +
                          "] AS " + *edge_type + "[])) AS _graph_walk");
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
  for (const auto* element : input) {
    if (element->filter_expr() != nullptr) {
      const auto expression = GraphFilter(*element->filter_expr(), scope, result.columns);
      if (!expression) {
        return std::nullopt;
      }
      filters.push_back(*expression);
    }
  }
  if (!filters.empty()) {
    result.sql = "SELECT q.*" + result.From() + " WHERE " + Join(filters, " AND ");
  }
  result.sql = "SELECT q.*, " + ColumnName(elements.front()->column_list(0).column_id()) +
               " AS _graph_head, " + ColumnName(elements.back()->column_list(0).column_id()) +
               " AS _graph_tail, CAST([" + Join(path_nodes, ", ") +
               "] AS VARCHAR[]) AS _graph_nodes, CAST([" + Join(path_edges, ", ") +
               "] AS VARCHAR[]) AS _graph_edges" + result.From();
  return result;
}

// A path's boundary keeps the original node table candidates, but uses the column
// created for its endpoint when the path is quantified.
std::unique_ptr<googlesql::ResolvedGraphNodeScan> BoundaryNode(
    const googlesql::ResolvedGraphPathScanBase& part, bool head, const Scope& scope) {
  const auto* node = &part;
  auto column = part.column_list(0);
  bool nullable = false;
  if (part.Is<googlesql::ResolvedGraphPathScan>()) {
    const auto& path = *part.GetAs<googlesql::ResolvedGraphPathScan>();
    column = head ? path.head() : path.tail();
  }
  while (node->Is<googlesql::ResolvedGraphPathScan>()) {
    const auto& path = *node->GetAs<googlesql::ResolvedGraphPathScan>();
    if (path.quantifier() != nullptr &&
        path.quantifier()->lower_bound()->Is<googlesql::ResolvedLiteral>()) {
      const auto& lower =
          path.quantifier()->lower_bound()->GetAs<googlesql::ResolvedLiteral>()->value();
      nullable = nullable || (!lower.is_null() && lower.int64_value() == 0);
    }
    node = head ? path.input_scan_list().front().get() : path.input_scan_list().back().get();
  }
  auto result = googlesql::MakeResolvedGraphNodeScan();
  result->set_column_list({column});
  if (nullable && scope.property_graph != nullptr) {
    // A zero-length subpath can connect at a node whose label does not match
    // either endpoint inside the repeated pattern.
    absl::flat_hash_set<const googlesql::GraphNodeTable*> tables;
    if (scope.property_graph->GetNodeTables(tables).ok()) {
      result->set_target_element_table_list({tables.begin(), tables.end()});
    }
  } else {
    result->set_target_element_table_list(
        node->GetAs<googlesql::ResolvedGraphElementScan>()->target_element_table_list());
  }
  return result;
}

std::string AppendNodes(const std::string& left, const std::string& right) {
  // Adjacent paths share one endpoint, including a zero-edge path.
  return "list_concat(" + left + ", list_slice(" + right + ", 2, length(" + right + ")))";
}

std::string AppendWalk(const std::string& left, const std::string& right) {
  return "struct_pack(\"$nodes\" := " + AppendNodes(left + ".\"$nodes\"", right + ".\"$nodes\"") +
         ", \"$edges\" := list_concat(" + left + ".\"$edges\", " + right + ".\"$edges\"))";
}

std::optional<Relation> GraphPath(const googlesql::ResolvedGraphPathScan& path, const Scope& scope,
                                  const googlesql::GraphPathType* path_type = nullptr) {
  if (path.path_cost() != nullptr || !path.hint_list().empty() || !path.path_hint_list().empty()) {
    return Unsupported(scope, "graph costs or hints");
  }
  if (path.search_prefix() != nullptr &&
      (path.search_prefix()->path_count() != nullptr ||
       (path.search_prefix()->type() != googlesql::ResolvedGraphPathSearchPrefix::ANY &&
        path.search_prefix()->type() != googlesql::ResolvedGraphPathSearchPrefix::SHORTEST))) {
    return Unsupported(scope, "this graph search prefix");
  }
  const auto mode = path.path_mode() == nullptr ? googlesql::ResolvedGraphPathMode::WALK
                                                : path.path_mode()->path_mode();
  if (mode != googlesql::ResolvedGraphPathMode::WALK &&
      mode != googlesql::ResolvedGraphPathMode::PATH_MODE_UNSPECIFIED &&
      mode != googlesql::ResolvedGraphPathMode::TRAIL &&
      mode != googlesql::ResolvedGraphPathMode::ACYCLIC) {
    return Unsupported(scope, "this graph path mode");
  }
  if (path_type == nullptr && path.path() != nullptr) {
    path_type = path.path()->column().type()->AsGraphPath();
  }
  std::vector<const googlesql::ResolvedGraphElementScan*> elements;
  for (const auto& part : path.input_scan_list()) {
    if (part->Is<googlesql::ResolvedGraphElementScan>()) {
      elements.push_back(part->GetAs<googlesql::ResolvedGraphElementScan>());
    }
  }
  std::optional<Relation> result;
  if (elements.size() == path.input_scan_list().size()) {
    result = FlatGraphPath(elements, path_type, scope);
  } else {
    // Compose subpaths and element patterns by identity. An edge gets its own
    // endpoint rows so references to non-key columns still resolve correctly.
    std::vector<Relation> pieces;
    for (size_t i = 0; i < path.input_scan_list().size(); ++i) {
      const auto& part = *path.input_scan_list().at(i);
      std::optional<Relation> piece;
      if (part.Is<googlesql::ResolvedGraphPathScan>()) {
        piece = GraphPath(*part.GetAs<googlesql::ResolvedGraphPathScan>(), scope, path_type);
      } else if (part.Is<googlesql::ResolvedGraphNodeScan>()) {
        piece = FlatGraphPath({part.GetAs<googlesql::ResolvedGraphNodeScan>()}, path_type, scope);
      } else if (part.Is<googlesql::ResolvedGraphEdgeScan>() && i > 0 &&
                 i + 1 < path.input_scan_list().size()) {
        auto left = BoundaryNode(*path.input_scan_list().at(i - 1), false, scope);
        auto right = BoundaryNode(*path.input_scan_list().at(i + 1), true, scope);
        piece =
            FlatGraphPath({left.get(), part.GetAs<googlesql::ResolvedGraphEdgeScan>(), right.get()},
                          path_type, scope);
        if (piece) {
          // Endpoint values belong to the adjacent pieces, not to this edge.
          piece->columns.erase(left->column_list(0).column_id());
          piece->columns.erase(right->column_list(0).column_id());
        }
      } else {
        return Unsupported(scope, "this nested graph path");
      }
      if (!piece) {
        return std::nullopt;
      }
      pieces.push_back(std::move(*piece));
    }
    Relation joined;
    std::vector<std::string> projections;
    std::string from;
    std::string nodes;
    std::string edges;
    std::string walk;
    for (size_t i = 0; i < pieces.size(); ++i) {
      const auto alias = std::format("s{}", i);
      from += std::format("{}{}) AS {}", i == 0 ? " FROM (" : " JOIN (", pieces.at(i).sql, alias);
      if (i > 0) {
        from += std::format(R"( ON s{}._graph_tail."$id" = {}._graph_head."$id")", i - 1, alias);
      }
      for (const auto& [id, unused] : pieces.at(i).columns) {
        projections.push_back(alias + "." + ColumnName(id) + " AS " + ColumnName(id));
        joined.columns.emplace(id, "q." + ColumnName(id));
      }
      const auto next_nodes = alias + "._graph_nodes";
      const auto next_edges = alias + "._graph_edges";
      nodes = i == 0 ? next_nodes : AppendNodes(nodes, next_nodes);
      edges = i == 0 ? next_edges : std::format("list_concat({}, {})", edges, next_edges);
      if (path_type != nullptr) {
        const auto next_walk = alias + "._graph_walk";
        walk = i == 0 ? next_walk : AppendWalk(std::format("({})", walk), next_walk);
      }
    }
    projections.emplace_back("s0._graph_head AS _graph_head");
    projections.push_back("s" + std::to_string(pieces.size() - 1) + "._graph_tail AS _graph_tail");
    projections.push_back(nodes + " AS _graph_nodes");
    projections.push_back(edges + " AS _graph_edges");
    if (path_type != nullptr) {
      projections.push_back(walk + " AS _graph_walk");
    }
    joined.sql = "SELECT " + Join(projections, ", ") + from;
    result = std::move(joined);
  }
  if (!result) {
    return std::nullopt;
  }
  if (path.filter_expr() != nullptr) {
    const auto filter = GraphFilter(*path.filter_expr(), scope, result->columns);
    if (!filter) {
      return std::nullopt;
    }
    result->sql = "SELECT q.*" + result->From() + " WHERE " + *filter;
  }
  if (path.quantifier() != nullptr) {
    const auto& quantifier = *path.quantifier();
    // BigQuery documents bounded quantification. Do not invent a stopping point
    // for an unbounded walk, or silently lose its repeated edges and nodes.
    if (quantifier.upper_bound() == nullptr ||
        !quantifier.lower_bound()->Is<googlesql::ResolvedLiteral>() ||
        !quantifier.upper_bound()->Is<googlesql::ResolvedLiteral>()) {
      return Unsupported(scope, "graph quantifiers without literal bounds");
    }
    const auto& lower = quantifier.lower_bound()->GetAs<googlesql::ResolvedLiteral>()->value();
    const auto& upper = quantifier.upper_bound()->GetAs<googlesql::ResolvedLiteral>()->value();
    if (lower.is_null() || upper.is_null() || lower.int64_value() < 0 ||
        upper.int64_value() < lower.int64_value()) {
      return Unsupported(scope, "graph quantifiers with invalid bounds");
    }
    const auto body = QuoteIdentifier(scope.context.FreshName("graph_step"));
    const auto recursive = QuoteIdentifier(scope.context.FreshName("graph_walk"));
    std::vector<std::string> initial = {
        "b._graph_head",
        "b._graph_tail",
        "b._graph_nodes",
        "b._graph_edges",
    };
    std::vector<std::string> next = {
        "r._graph_head",
        "b._graph_tail",
        AppendNodes("r._graph_nodes", "b._graph_nodes"),
        "list_concat(r._graph_edges, b._graph_edges)",
    };
    std::vector<std::string> names = {"_graph_head", "_graph_tail", "_graph_nodes", "_graph_edges"};
    Columns columns;
    for (const auto& group : path.group_variable_list()) {
      names.push_back(ColumnName(group->array().column_id()));
      initial.push_back("[b." + ColumnName(group->element().column_id()) + "]");
      next.push_back("list_append(r." + names.back() + ", b." +
                     ColumnName(group->element().column_id()) + ")");
      columns.emplace(group->array().column_id(), "q." + names.back());
    }
    if (path_type != nullptr) {
      names.emplace_back("_graph_walk");
      initial.emplace_back("b._graph_walk");
      next.push_back(AppendWalk("r._graph_walk", "b._graph_walk"));
    }
    names.emplace_back("_graph_repetitions");
    initial.emplace_back("1::BIGINT");
    next.emplace_back("r._graph_repetitions + 1");
    result->sql = "WITH RECURSIVE " + body + " AS MATERIALIZED (" + result->sql + "), " +
                  recursive + "(" + Join(names, ", ") + ") AS (SELECT " + Join(initial, ", ") +
                  " FROM " + body + " b UNION ALL SELECT " + Join(next, ", ") + " FROM " +
                  recursive + " r JOIN " + body +
                  R"( b ON r._graph_tail."$id" = b._graph_head."$id")" +
                  " WHERE r._graph_repetitions < " + std::to_string(upper.int64_value()) +
                  ") SELECT *, _graph_head AS " + ColumnName(path.head().column_id()) +
                  ", _graph_tail AS " + ColumnName(path.tail().column_id()) + " FROM " + recursive +
                  " WHERE _graph_repetitions BETWEEN " + std::to_string(lower.int64_value()) +
                  " AND " + std::to_string(upper.int64_value());
    columns.emplace(path.head().column_id(), "q." + ColumnName(path.head().column_id()));
    columns.emplace(path.tail().column_id(), "q." + ColumnName(path.tail().column_id()));
    result->columns = std::move(columns);
    if (lower.int64_value() == 0) {
      absl::flat_hash_set<const googlesql::GraphNodeTable*> tables;
      if (scope.property_graph == nullptr || !scope.property_graph->GetNodeTables(tables).ok()) {
        return Unsupported(scope, "graph nodes for zero repetitions");
      }
      const auto head_type =
          DuckDbType(path.head().type(), nullptr, scope.context.timestamp_precision);
      const auto tail_type =
          DuckDbType(path.tail().type(), nullptr, scope.context.timestamp_precision);
      if (!head_type || !tail_type) {
        return Unsupported(scope, "graph endpoint types");
      }
      // Zero repetitions impose no labels or filters from inside the quantified
      // pattern: every graph node can connect the preceding and following parts.
      std::vector<std::string> zero_rows;
      for (const auto* table : tables) {
        auto node = googlesql::MakeResolvedGraphNodeScan();
        const auto& head = path.head();
        // Zero repetitions can start on a different table than the repeated
        // pattern. Preserve its properties in the enclosing path's common type.
        node->set_column_list({
            googlesql::ResolvedColumn(head.column_id(), head.table_name_id(), head.name_id(),
                                      path_type == nullptr ? head.type() : path_type->node_type()),
        });
        node->set_target_element_table_list({table});
        const auto zero = FlatGraphPath({node.get()}, path_type, scope);
        if (!zero) {
          return std::nullopt;
        }
        std::vector<std::string> values = {
            "CAST(q._graph_head AS " + *head_type + ")",
            "CAST(q._graph_tail AS " + *tail_type + ")",
            "q._graph_nodes",
            "q._graph_edges",
        };
        for (const auto& group : path.group_variable_list()) {
          const auto type =
              DuckDbType(group->array().type(), nullptr, scope.context.timestamp_precision);
          if (!type) {
            return Unsupported(scope, "graph group variable type");
          }
          values.push_back("CAST([] AS " + *type + ")");
        }
        if (path_type != nullptr) {
          values.emplace_back("q._graph_walk");
        }
        values.emplace_back("0::BIGINT");
        values.push_back("CAST(q._graph_head AS " + *head_type + ")");
        values.push_back("CAST(q._graph_tail AS " + *tail_type + ")");
        zero_rows.push_back("SELECT " + Join(values, ", ") + zero->From());
      }
      result->sql = "(" + result->sql + ") UNION ALL " + Join(zero_rows, " UNION ALL ");
    }
  }
  if (mode == googlesql::ResolvedGraphPathMode::TRAIL ||
      mode == googlesql::ResolvedGraphPathMode::ACYCLIC) {
    const std::string values =
        mode == googlesql::ResolvedGraphPathMode::TRAIL ? "q._graph_edges" : "q._graph_nodes";
    result->sql = "SELECT q.*" + result->From() + " WHERE list_unique(" + values + ") = length(" +
                  values + ")";
  }
  if (path.search_prefix() != nullptr) {
    // Select independently for each pair of element identities, after matching
    // the entire path and its subpath modes. Count edges, not repetitions.
    // MATCH's WHERE and joins to other patterns are applied by GraphScan later.
    const std::string order =
        path.search_prefix()->type() == googlesql::ResolvedGraphPathSearchPrefix::SHORTEST
            ? " ORDER BY length(q._graph_edges)"
            : "";
    result->sql =
        "SELECT q.*" + result->From() +
        R"( QUALIFY row_number() OVER (PARTITION BY q._graph_head."$id", q._graph_tail."$id")" +
        order + ") = 1";
  }
  if (path.path() != nullptr) {
    const auto& column = path.path()->column();
    const auto type = DuckDbType(column.type(), nullptr, scope.context.timestamp_precision);
    if (!type) {
      return Unsupported(scope, "graph path type");
    }
    result->sql = "SELECT q.*, CAST(_graph_walk AS " + *type + ") AS " +
                  ColumnName(column.column_id()) + result->From();
    result->columns.emplace(column.column_id(), "q." + ColumnName(column.column_id()));
  }
  return result;
}

}  // namespace

std::optional<Relation> GraphScan(const googlesql::ResolvedGraphScan& scan, const Scope& scope) {
  if (scope.graph_name.empty() || scan.input_scan_list().empty()) {
    return Unsupported(scope, "graph patterns without a graph or paths");
  }
  const auto input = scan.input_scan() == nullptr
                         ? std::optional<Relation>(
                               Relation{.sql = "SELECT 1 AS _unit", .columns = {}, .ordering = {}})
                         : Scan(*scan.input_scan(), scope);
  if (!input) {
    return std::nullopt;
  }
  const auto left = QuoteIdentifier(scope.context.FreshName("graph_input"));
  Columns input_columns;
  std::vector<std::string> outputs;
  for (const auto& [id, unused] : input->columns) {
    input_columns.emplace(id, left + "." + ColumnName(id));
    outputs.push_back(left + "." + ColumnName(id) + " AS " + ColumnName(id));
  }
  Relation result;
  std::vector<std::string> paths;
  std::vector<std::string> projections;
  for (const auto& path : scan.input_scan_list()) {
    const auto match = GraphPath(*path, scope);
    if (!match) {
      return std::nullopt;
    }
    const auto alias = QuoteIdentifier("p" + std::to_string(paths.size()));
    paths.push_back("(" + match->sql + ") AS " + alias);
    for (const auto& [id, unused] : match->columns) {
      projections.push_back(alias + "." + ColumnName(id) + " AS " + ColumnName(id));
      result.columns.emplace(id, "q." + ColumnName(id));
      outputs.push_back("graph_match." + ColumnName(id) + " AS " + ColumnName(id));
    }
  }
  result.sql = "SELECT " + Join(projections, ", ") + " FROM " + Join(paths, " CROSS JOIN ");
  Columns filter_columns = input_columns;
  for (const auto& [id, unused] : result.columns) {
    filter_columns.emplace(id, "graph_match." + ColumnName(id));
  }
  std::string condition = "TRUE";
  if (scan.filter_expr() != nullptr) {
    const auto filter = GraphFilter(*scan.filter_expr(), scope, filter_columns);
    if (!filter) {
      return std::nullopt;
    }
    condition = *filter;
  }
  // The entire pattern is the right side of the join. Its WHERE is a join condition,
  // so an optional match preserves the input row when every candidate is rejected.
  result.sql = "SELECT " + Join(outputs, ", ") + " FROM (" + input->sql + ") AS " + left +
               (scan.optional() ? " LEFT JOIN (" : " INNER JOIN (") + result.sql +
               ") AS graph_match ON " + condition;
  for (const auto& [id, unused] : input->columns) {
    result.columns.emplace(id, "q." + ColumnName(id));
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
  graph_scope.property_graph = scan.property_graph();
  return Scan(*scan.input_scan(), graph_scope);
}
}  // namespace bigquery_emulator_duckdb::translator
