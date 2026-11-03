#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_set.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/match.h"
#include "absl/strings/str_join.h"
#include "googlesql/public/analyzer_options.h"
#include "googlesql/public/catalog.h"
#include "googlesql/public/function_signature.h"
#include "googlesql/public/parse_resume_location.h"
#include "googlesql/public/property_graph.h"
#include "googlesql/public/simple_catalog.h"
#include "googlesql/public/strings.h"
#include "googlesql/public/table_valued_function.h"
#include "googlesql/public/templated_sql_tvf.h"
#include "googlesql/public/type.h"
#include "googlesql/public/value.h"
#include "src/analyzer_functions/declarations.h"

namespace bigquery_emulator_duckdb::analyzer_functions {
namespace {

const std::vector<std::string>& ArgumentNames() {
  static const auto* const kNames =
      new std::vector<std::string>{"graph_name", "start_node", "flatten_results"};
  return *kNames;
}

googlesql::FunctionSignature Signature() {
  googlesql::FunctionArgumentTypeOptions start_node(googlesql::FunctionArgumentType::OPTIONAL);
  start_node.set_argument_name("start_node", googlesql::kNamedOnly);
  start_node.set_default(googlesql::Value::NullString());
  googlesql::FunctionArgumentTypeOptions flatten_results(googlesql::FunctionArgumentType::OPTIONAL);
  flatten_results.set_argument_name("flatten_results", googlesql::kNamedOnly);
  flatten_results.set_default(googlesql::Value::Bool(true));
  return {
      googlesql::ARG_KIND_RELATION,
      {
          googlesql::FunctionArgumentType(googlesql::types::StringType()),
          googlesql::FunctionArgumentType(googlesql::types::StringType(), start_node),
          googlesql::FunctionArgumentType(googlesql::types::BoolType(), flatten_results),
      },
      /*context_id=*/int64_t{0},
  };
}

absl::Status Unsupported(const std::string& what) {
  return absl::InvalidArgumentError("The emulator does not support GRAPH_EXPAND " + what);
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): a set and its subset.
bool Includes(const std::vector<int>& all, const std::vector<int>& some) {
  return std::ranges::all_of(
      some, [&](int column) { return std::ranges::find(all, column) != all.end(); });
}

struct Property {
  std::string name;
  std::string sql;
  auto operator<=>(const Property&) const = default;
};

// The properties of an element table, ordered by name as BigQuery's documented example
// orders the columns of each table.
absl::StatusOr<std::vector<Property>> Properties(const googlesql::GraphElementTable& table) {
  absl::flat_hash_set<const googlesql::GraphPropertyDefinition*> definitions;
  if (auto status = table.GetPropertyDefinitions(definitions); !status.ok()) {
    return status;
  }
  std::vector<Property> properties;
  std::ranges::transform(definitions, std::back_inserter(properties), [](const auto* definition) {
    return Property{definition->GetDeclaration().Name(), std::string(definition->expression_sql())};
  });
  std::ranges::sort(properties);
  return properties;
}

// An edge of the schema relationship graph: each row of `from` corresponds to at most one row of
// `to`, which the edge table's `from_columns` reference and its `to_columns` identify.
struct SchemaEdge {
  const googlesql::GraphEdgeTable* table;
  const googlesql::GraphNodeTable* from;
  const googlesql::GraphNodeTable* to;
  const googlesql::GraphNodeTableReference* from_reference;
  const googlesql::GraphNodeTableReference* to_reference;
};

// The edge an edge table gives the schema relationship graph, if its key makes the relationship
// many-to-one in exactly one direction.
std::optional<SchemaEdge> RelationshipEdge(const googlesql::GraphEdgeTable& table) {
  const auto* source = table.GetSourceNodeTable();
  const auto* destination = table.GetDestNodeTable();
  // Each row of `one` has at most one edge, which leads to at most one row of the other's node.
  const auto many_to_one = [&](const googlesql::GraphNodeTableReference& one,
                               const googlesql::GraphNodeTableReference& other) {
    return Includes(one.GetEdgeTableColumns(), table.GetKeyColumns()) &&
           Includes(other.GetNodeTableColumns(), other.GetReferencedNodeTable()->GetKeyColumns());
  };
  const bool from_source = many_to_one(*source, *destination);
  const bool from_destination = many_to_one(*destination, *source);
  if (from_source == from_destination) {
    return std::nullopt;
  }
  if (from_source) {
    return SchemaEdge{
        .table = &table,
        .from = source->GetReferencedNodeTable(),
        .to = destination->GetReferencedNodeTable(),
        .from_reference = source,
        .to_reference = destination,
    };
  }
  return SchemaEdge{
      .table = &table,
      .from = destination->GetReferencedNodeTable(),
      .to = source->GetReferencedNodeTable(),
      .from_reference = destination,
      .to_reference = source,
  };
}

// Whether the edge's rows are those of its `from` table, whose properties include its own.
absl::StatusOr<bool> Inlined(const SchemaEdge& edge) {
  if (edge.table->GetTable() != edge.from->GetTable() ||
      edge.from_reference->GetEdgeTableColumns() != edge.from_reference->GetNodeTableColumns()) {
    return false;
  }
  const auto edge_properties = Properties(*edge.table);
  const auto node_properties = Properties(*edge.from);
  if (!edge_properties.ok() || !node_properties.ok()) {
    return edge_properties.ok() ? node_properties.status() : edge_properties.status();
  }
  return std::ranges::includes(*node_properties, *edge_properties);
}

// The prefix of an element table's columns: its default label, the only label it may have.
absl::StatusOr<std::string> DefaultLabel(const googlesql::GraphElementTable& table) {
  absl::flat_hash_set<const googlesql::GraphElementLabel*> labels;
  if (auto status = table.GetLabels(labels); !status.ok()) {
    return status;
  }
  if (table.HasDynamicLabel() || table.HasDynamicProperties() || labels.size() != 1 ||
      !absl::EqualsIgnoreCase((*labels.begin())->Name(), table.Name())) {
    return absl::InvalidArgumentError("GRAPH_EXPAND requires element table " + table.Name() +
                                      " to use only its default label");
  }
  return (*labels.begin())->Name();
}

std::string ColumnName(const googlesql::GraphElementTable& table, int column) {
  return table.GetTable()->GetColumn(column)->Name();
}

// The GoogleSQL query that expands the graph: its node tables LEFT JOINed from the root along
// the schema relationship graph, with each table's properties prefixed by its label.
absl::StatusOr<std::string> ExpansionQuery(
    googlesql::Catalog* catalog, const googlesql::AnalyzerOptions& options,
    const std::vector<googlesql::TVFInputArgumentType>& arguments) {
  std::vector<googlesql::Value> literals;
  for (const auto& argument : arguments) {
    const auto scalar = argument.GetScalarArgType();
    if (!scalar.ok() || scalar->literal_value() == nullptr) {
      return Unsupported("arguments that are not literals");
    }
    literals.push_back(*scalar->literal_value());
  }
  if (literals.at(0).is_null()) {
    return absl::InvalidArgumentError("GRAPH_EXPAND requires a graph name");
  }
  if (literals.at(2).is_null() || !literals.at(2).bool_value()) {
    return Unsupported("with flatten_results => FALSE");
  }
  const googlesql::PropertyGraph* graph = nullptr;
  if (auto status = catalog->FindPropertyGraph({literals.at(0).string_value()}, graph,
                                               options.find_options());
      !status.ok()) {
    return status;
  }
  absl::flat_hash_set<const googlesql::GraphNodeTable*> node_set;
  absl::flat_hash_set<const googlesql::GraphEdgeTable*> edge_set;
  if (auto status = graph->GetNodeTables(node_set); !status.ok()) {
    return status;
  }
  if (auto status = graph->GetEdgeTables(edge_set); !status.ok()) {
    return status;
  }
  // Ordered by name, so that errors and joins do not depend on hash order.
  const auto by_name = [](const auto* a, const auto* b) { return a->Name() < b->Name(); };
  std::vector<const googlesql::GraphNodeTable*> nodes(node_set.begin(), node_set.end());
  std::vector<const googlesql::GraphEdgeTable*> edge_tables(edge_set.begin(), edge_set.end());
  std::ranges::sort(nodes, by_name);
  std::ranges::sort(edge_tables, by_name);
  if (nodes.empty()) {
    return absl::InvalidArgumentError("GRAPH_EXPAND requires a graph with a node table");
  }
  bool any_property = false;
  std::map<const googlesql::GraphElementTable*, std::string> labels;
  for (const googlesql::GraphElementTable* table : nodes) {
    auto label = DefaultLabel(*table);
    if (!label.ok()) {
      return label.status();
    }
    labels.emplace(table, *std::move(label));
  }
  std::vector<SchemaEdge> edges;
  for (const googlesql::GraphElementTable* table : edge_tables) {
    if (const auto label = DefaultLabel(*table); !label.ok()) {
      return label.status();
    }
    if (auto edge = RelationshipEdge(*table->AsEdgeTable())) {
      edges.push_back(*edge);
    }
  }
  for (const auto* table : nodes) {
    auto properties = Properties(*table);
    if (!properties.ok()) {
      return properties.status();
    }
    any_property = any_property || !properties->empty();
  }
  for (const auto* table : edge_tables) {
    auto properties = Properties(*table);
    if (!properties.ok()) {
      return properties.status();
    }
    any_property = any_property || !properties->empty();
  }
  if (!any_property) {
    return absl::InvalidArgumentError("GRAPH_EXPAND requires a graph with a property");
  }

  const googlesql::GraphNodeTable* root = nullptr;
  if (!literals.at(1).is_null()) {
    const auto found = std::ranges::find_if(nodes, [&](const auto* node) {
      return absl::EqualsIgnoreCase(node->Name(), literals.at(1).string_value());
    });
    if (found == nodes.end()) {
      return absl::InvalidArgumentError("GRAPH_EXPAND start_node " + literals.at(1).string_value() +
                                        " is not a node table");
    }
    root = *found;
  } else {
    std::vector<const googlesql::GraphNodeTable*> roots;
    for (const auto* node : nodes) {
      if (std::ranges::none_of(edges, [&](const SchemaEdge& edge) { return edge.to == node; })) {
        roots.push_back(node);
      }
    }
    if (roots.size() != 1) {
      return absl::InvalidArgumentError(
          roots.empty() ? "GRAPH_EXPAND requires a graph with a root node table"
                        : "GRAPH_EXPAND requires start_node for a graph with multiple root node "
                          "tables");
    }
    root = roots.front();
  }
  // The tables reachable from the root, which must form a tree.
  std::set<const googlesql::GraphNodeTable*> reachable = {root};
  std::vector<const googlesql::GraphNodeTable*> pending = {root};
  while (!pending.empty()) {
    const auto* node = pending.back();
    pending.pop_back();
    for (const SchemaEdge& edge : edges) {
      if (edge.from == node && reachable.insert(edge.to).second) {
        pending.push_back(edge.to);
      }
    }
  }
  if (literals.at(1).is_null() && reachable.size() != nodes.size()) {
    return absl::InvalidArgumentError(
        "GRAPH_EXPAND requires every node table to be reachable from the root node table");
  }
  std::map<const googlesql::GraphNodeTable*, int> in_degree;
  std::map<const googlesql::GraphNodeTable*, std::vector<const SchemaEdge*>> children;
  for (const SchemaEdge& edge : edges) {
    if (reachable.contains(edge.from)) {
      ++in_degree[edge.to];
      children[edge.from].push_back(&edge);
    }
  }
  if (in_degree[root] != 0) {
    return absl::InvalidArgumentError("GRAPH_EXPAND requires an acyclic graph");
  }
  if (std::ranges::any_of(in_degree, [](const auto& entry) { return entry.second > 1; })) {
    return absl::InvalidArgumentError(
        "GRAPH_EXPAND requires every node table to have at most one incoming relationship");
  }

  // BigQuery documents the columns of a chain of tables joined through inlined edges.
  std::vector<const googlesql::GraphNodeTable*> chain = {root};
  std::vector<const SchemaEdge*> joins;
  for (auto next = children.find(root); next != children.end();
       next = children.find(chain.back())) {
    if (next->second.size() > 1) {
      return Unsupported("of a node table related to more than one node table");
    }
    const SchemaEdge& edge = *next->second.front();
    const auto inlined = Inlined(edge);
    if (!inlined.ok()) {
      return inlined.status();
    }
    if (!*inlined) {
      return Unsupported("of an edge table with rows or properties of its own");
    }
    joins.push_back(&edge);
    chain.push_back(edge.to);
  }

  // Each table's subquery exposes its properties as p<i> and the columns its joins compare as
  // c<i>, so that property and column names cannot collide.
  std::vector<std::string> outputs;
  std::vector<std::string> from;
  for (std::size_t i = 0; i < chain.size(); ++i) {
    const googlesql::GraphNodeTable& node = *chain.at(i);
    const std::string alias = "t" + std::to_string(i);
    std::vector<std::string> projections;
    const auto properties = Properties(node);
    if (!properties.ok()) {
      return properties.status();
    }
    for (std::size_t p = 0; p < properties->size(); ++p) {
      projections.push_back("(" + properties->at(p).sql + ") AS p" + std::to_string(p));
      outputs.push_back(
          alias + ".p" + std::to_string(p) + " AS " +
          googlesql::ToIdentifierLiteral(labels.at(&node) + "_" + properties->at(p).name));
    }
    std::set<int> columns;
    std::vector<std::string> conditions;
    if (i > 0) {
      // The joined table's referenced columns equal the columns of the previous table that
      // reference them, since the edge's rows are the previous table's.
      const SchemaEdge& edge = *joins.at(i - 1);
      const auto& referenced = edge.to_reference->GetNodeTableColumns();
      const auto& referencing = edge.to_reference->GetEdgeTableColumns();
      for (std::size_t c = 0; c < referenced.size(); ++c) {
        columns.insert(referenced.at(c));
        conditions.push_back(alias + ".c" + std::to_string(referenced.at(c)) + " = t" +
                             std::to_string(i - 1) + ".c" + std::to_string(referencing.at(c)));
      }
    }
    if (i + 1 < chain.size()) {
      const auto& referencing = joins.at(i)->to_reference->GetEdgeTableColumns();
      columns.insert(referencing.begin(), referencing.end());
    }
    std::ranges::transform(columns, std::back_inserter(projections), [&](int column) {
      return googlesql::ToIdentifierLiteral(ColumnName(node, column)) + " AS c" +
             std::to_string(column);
    });
    if (projections.empty()) {
      projections.emplace_back("1 AS unit");
    }
    const std::string table = "(SELECT " + absl::StrJoin(projections, ", ") + " FROM " +
                              googlesql::ToIdentifierLiteral(node.GetTable()->FullName()) +
                              ") AS " + alias;
    from.push_back(i == 0 ? table
                          : "LEFT JOIN " + table + " ON " + absl::StrJoin(conditions, " AND "));
  }
  if (outputs.empty()) {
    return absl::InvalidArgumentError("GRAPH_EXPAND requires a property on an expanded table");
  }
  return "SELECT " + absl::StrJoin(outputs, ", ") + " FROM " + absl::StrJoin(from, " ");
}

// BigQuery's GRAPH_EXPAND, which GoogleSQL does not declare. Each call resolves the query that
// ExpansionQuery builds for its graph as the body of a templated SQL TVF, which the analyzer's
// SQL TVF inliner then puts in place of the call.
class GraphExpand : public googlesql::TemplatedSQLTVF {
 public:
  GraphExpand()
      : TemplatedSQLTVF({"graph_expand"}, Signature(), ArgumentNames(),
                        googlesql::ParseResumeLocation::FromStringView("")) {}

 protected:
  absl::Status ResolveInternal(
      const googlesql::AnalyzerOptions* analyzer_options,
      const std::vector<googlesql::TVFInputArgumentType>& input_arguments,
      const googlesql::FunctionSignature& concrete_signature, googlesql::Catalog* catalog,
      googlesql::TypeFactory* type_factory,
      std::shared_ptr<googlesql::TVFSignature>* tvf_signature) const final {
    auto query = ExpansionQuery(catalog, *analyzer_options, input_arguments);
    if (!query.ok()) {
      return query.status();
    }
    // The catalog shares this declaration across statements, so each call resolves its own body.
    const googlesql::TemplatedSQLTVF body(
        {"graph_expand"}, Signature(), ArgumentNames(),
        googlesql::ParseResumeLocation::FromString(*std::move(query)));
    return body.Resolve(analyzer_options, input_arguments, concrete_signature, catalog,
                        type_factory, tvf_signature);
  }
};

}  // namespace

void AddGraphExpand(googlesql::SimpleCatalog& catalog) {
  catalog.AddOwnedTableValuedFunction(std::make_unique<GraphExpand>());
}

}  // namespace bigquery_emulator_duckdb::analyzer_functions
