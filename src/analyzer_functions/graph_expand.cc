#include "src/analyzer_functions/graph_expand.h"

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
#include "absl/strings/ascii.h"
#include "absl/strings/match.h"
#include "absl/strings/str_join.h"
#include "absl/strings/string_view.h"
#include "googlesql/public/analyzer.h"
#include "googlesql/public/analyzer_options.h"
#include "googlesql/public/analyzer_output.h"
#include "googlesql/public/catalog.h"
#include "googlesql/public/function.h"
#include "googlesql/public/function_signature.h"
#include "googlesql/public/measure_expression.h"
#include "googlesql/public/property_graph.h"
#include "googlesql/public/simple_catalog.h"
#include "googlesql/public/strings.h"
#include "googlesql/public/table_valued_function.h"
#include "googlesql/public/type.h"
#include "googlesql/public/types/type_factory.h"
#include "googlesql/public/value.h"
#include "googlesql/resolved_ast/resolved_ast.h"
#include "googlesql/resolved_ast/resolved_ast_builder.h"
#include "googlesql/resolved_ast/resolved_ast_deep_copy_visitor.h"
#include "googlesql/resolved_ast/resolved_ast_rewrite_visitor.h"
#include "googlesql/resolved_ast/resolved_node_kind.pb.h"
#include "src/analyzer_functions/declarations.h"
#include "src/analyzer_functions/functions.h"

namespace bigquery_emulator_duckdb::analyzer_functions {
namespace {

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
  bool is_measure = false;
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
    const auto& declaration = definition->GetDeclaration();
    return Property{
        .name = declaration.Name(),
        .sql = std::string(definition->expression_sql()),
        .is_measure = declaration.kind() == googlesql::GraphPropertyDeclaration::Kind::kMeasure,
    };
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

// Whether the edge's rows are those of its `from` table, whose properties include its own: an
// edge the documentation inlines, omitting its columns. BigQuery inlined such an edge only where
// its table has no columns other than those that reference its node tables (HasOtherColumns),
// whichever endpoint references the `from` table, whether a foreign key relates the tables and
// whatever the other node table's properties, and joined it otherwise, as observed in
// tests/e2e/goclient/testdata/scalars/graph/graph_expand.txt.
// third_party/bigquery_docs/reference/standard-sql/graph-sql-queries.md
absl::StatusOr<bool> SharesNodeRows(const SchemaEdge& edge) {
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

// Whether the edge table has columns other than those that reference its node tables.
bool HasOtherColumns(const SchemaEdge& edge) {
  std::set<int> references(edge.from_reference->GetEdgeTableColumns().begin(),
                           edge.from_reference->GetEdgeTableColumns().end());
  references.insert(edge.to_reference->GetEdgeTableColumns().begin(),
                    edge.to_reference->GetEdgeTableColumns().end());
  return std::cmp_greater(edge.table->GetTable()->NumColumns(), references.size());
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

// A table of the expansion, LEFT JOINed to the table of step `parent` where its `referenced`
// columns equal that table's `referencing` columns. `with_key` adds an edge table's key columns
// to its properties.
struct Step {
  const googlesql::GraphElementTable* table;
  std::size_t parent = 0;
  std::vector<int> referenced;
  std::vector<int> referencing;
  bool with_key = false;
};

using Children = std::map<const googlesql::GraphNodeTable*, std::vector<const SchemaEdge*>>;

// BigQuery joins siblings in edge declaration order, regardless of node declaration order.
absl::Status OrderRelated(std::vector<const SchemaEdge*>& related,
                          const std::vector<std::string>& order) {
  if (related.size() < 2) {
    return absl::OkStatus();
  }
  const auto position = [&](const googlesql::GraphElementTable* table) {
    return std::ranges::find_if(order,
                                [&](const std::string& name) {
                                  return absl::EqualsIgnoreCase(name, table->Name());
                                }) -
           order.begin();
  };
  const auto missing = static_cast<std::ptrdiff_t>(order.size());
  if (std::ranges::any_of(
          related, [&](const SchemaEdge* edge) { return position(edge->table) == missing; })) {
    return Unsupported("of a node table related to more than one node table");
  }
  std::ranges::sort(related, {}, [&](const SchemaEdge* edge) { return position(edge->table); });
  return absl::OkStatus();
}

// Adds the immediately related tables, keeping each non-inlined edge next to its destination.
absl::Status AddRelated(const googlesql::GraphNodeTable* node, std::size_t index,
                        const Children& children, const std::vector<std::string>& order,
                        std::vector<Step>& steps) {
  const auto found = children.find(node);
  if (found == children.end()) {
    return absl::OkStatus();
  }
  auto related = found->second;
  if (auto status = OrderRelated(related, order); !status.ok()) {
    return status;
  }
  for (const SchemaEdge* edge : related) {
    const auto shares = SharesNodeRows(*edge);
    if (!shares.ok()) {
      return shares.status();
    }
    std::size_t parent = index;
    if (!*shares || HasOtherColumns(*edge)) {
      steps.push_back({
          .table = edge->table,
          .parent = index,
          .referenced = edge->from_reference->GetEdgeTableColumns(),
          .referencing = edge->from_reference->GetNodeTableColumns(),
          .with_key = true,
      });
      parent = steps.size() - 1;
    }
    // The next node table joins the edge table's columns that reference it, which, since an
    // inlined edge's rows are its `from` table's, are that table's own columns.
    steps.push_back({
        .table = edge->to,
        .parent = parent,
        .referenced = edge->to_reference->GetNodeTableColumns(),
        .referencing = edge->to_reference->GetEdgeTableColumns(),
        .with_key = false,
    });
  }
  return absl::OkStatus();
}

std::string ColumnName(const googlesql::GraphElementTable& table, int column) {
  return table.GetTable()->GetColumn(column)->Name();
}

// A measure resolved against the properties of its element table: the expression's
// ExpressionColumns name the table's properties and the measures it derives from.
struct MeasureDefinition {
  std::string sql;
  const googlesql::ResolvedExpr* expr = nullptr;
  // The body output that holds each referenced name's value, by lowercase name.
  std::map<std::string, int> references;
  std::vector<int> keys;
};

struct ResolvedMeasure {
  std::string sql;
  const googlesql::ResolvedExpr* expr;
};

std::set<std::string> ReferencedNames(const googlesql::ResolvedExpr& expr) {
  std::vector<const googlesql::ResolvedNode*> columns;
  expr.GetDescendantsWithKinds({googlesql::RESOLVED_EXPRESSION_COLUMN}, &columns);
  std::set<std::string> names;
  std::ranges::transform(columns, std::inserter(names, names.end()), [](const auto* column) {
    return absl::AsciiStrToLower(
        column->template GetAs<googlesql::ResolvedExpressionColumn>()->name());
  });
  return names;
}

// Resolves the element table's measures, by lowercase name, as the graph's DDL does: against its
// other properties, so that a derived measure reads the measures it aggregates. A measure is
// resolved once those it derives from are, in as many rounds as the longest chain of them.
absl::StatusOr<std::map<std::string, ResolvedMeasure>> ResolveMeasures(
    const googlesql::GraphElementTable& element, const googlesql::AnalyzerOptions& options,
    googlesql::Catalog& catalog, googlesql::TypeFactory& type_factory,
    std::vector<std::unique_ptr<const googlesql::AnalyzerOutput>>& analyses) {
  absl::flat_hash_set<const googlesql::GraphPropertyDefinition*> definitions;
  if (auto status = element.GetPropertyDefinitions(definitions); !status.ok()) {
    return status;
  }
  std::map<std::string, const googlesql::Type*> scalars;
  std::map<std::string, std::string> pending;
  for (const auto* definition : definitions) {
    const auto& declaration = definition->GetDeclaration();
    if (declaration.kind() == googlesql::GraphPropertyDeclaration::Kind::kMeasure) {
      pending.emplace(declaration.Name(), definition->expression_sql());
    } else {
      scalars.emplace(declaration.Name(), declaration.Type());
    }
  }
  std::map<std::string, ResolvedMeasure> resolved;
  while (!pending.empty()) {
    googlesql::SimpleTable scope(element.Name());
    for (const auto& [name, type] : scalars) {
      if (auto status = scope.AddColumn(new googlesql::SimpleColumn(element.Name(), name, type),
                                        /*is_owned=*/true);
          !status.ok()) {
        return status;
      }
    }
    for (const auto& [name, measure] : resolved) {
      const auto type = type_factory.MakeMeasureType(measure.expr->type());
      if (!type.ok()) {
        return type.status();
      }
      if (auto status = scope.AddColumn(new googlesql::SimpleColumn(element.Name(), name, *type),
                                        /*is_owned=*/true);
          !status.ok()) {
        return status;
      }
    }
    absl::Status first_error;
    std::vector<std::string> done;
    for (const auto& [name, sql] : pending) {
      std::unique_ptr<const googlesql::AnalyzerOutput> analysis;
      googlesql::AnalyzerOptions measure_options(options.language());
      // Locate aggregates for UnrecognizedAggregate.
      measure_options.set_parse_location_record_type(
          googlesql::PARSE_LOCATION_RECORD_FULL_NODE_SCOPE);
      const auto expr = googlesql::AnalyzeMeasureExpression(sql, scope, catalog, type_factory,
                                                            measure_options, analysis);
      if (!expr.ok()) {
        if (first_error.ok()) {
          first_error = expr.status();
        }
        continue;
      }
      resolved.emplace(name, ResolvedMeasure{.sql = sql, .expr = *expr});
      analyses.push_back(std::move(analysis));
      done.push_back(name);
    }
    if (done.empty()) {
      return first_error;
    }
    for (const auto& name : done) {
      pending.erase(name);
    }
  }
  std::map<std::string, ResolvedMeasure> by_lowercase;
  for (auto& [name, measure] : resolved) {
    by_lowercase.emplace(absl::AsciiStrToLower(name), std::move(measure));
  }
  return by_lowercase;
}

// BigQuery accepts a measure that aggregates a property beside an aggregate of none, such as
// SUM(amount) + COUNT(1), but GRAPH_EXPAND then fails on that aggregate as a name, as observed in
// tests/e2e/goclient/testdata/scalars/graph/graph_expand_derived_measures.txt.
absl::Status UnrecognizedAggregate(const ResolvedMeasure& measure, const std::string& prefix) {
  std::vector<const googlesql::ResolvedNode*> calls;
  measure.expr->GetDescendantsWithKinds({googlesql::RESOLVED_AGGREGATE_FUNCTION_CALL}, &calls);
  if (measure.expr->Is<googlesql::ResolvedAggregateFunctionCall>()) {
    calls.push_back(measure.expr);
  }
  std::vector<std::pair<int, std::string>> unrecognized;
  for (const auto* call : calls) {
    std::vector<const googlesql::ResolvedNode*> columns;
    call->GetDescendantsWithKinds({googlesql::RESOLVED_EXPRESSION_COLUMN}, &columns);
    const auto* range = call->GetParseLocationRangeOrNULL();
    if (columns.empty() && range != nullptr) {
      const int start = range->start().GetByteOffset();
      unrecognized.emplace_back(start,
                                measure.sql.substr(start, range->end().GetByteOffset() - start));
    }
  }
  if (unrecognized.empty()) {
    return absl::OkStatus();
  }
  std::ranges::sort(unrecognized);
  return absl::InvalidArgumentError("Unrecognized name: `" + prefix + unrecognized.front().second +
                                    "`");
}

// A result column that a measure expression reads: its name, and, for a measure it derives
// from, its type. The measure rewriter identifies a measure by its type object.
struct ResultColumn {
  std::string name;
  const googlesql::Type* measure_type = nullptr;
};

// Renames a measure expression's ExpressionColumns to the result columns that hold their values.
class RenameExpressionColumns : public googlesql::ResolvedASTRewriteVisitor {
 public:
  explicit RenameExpressionColumns(const std::map<std::string, ResultColumn>& names)
      : names_(names) {}

 protected:
  absl::StatusOr<std::unique_ptr<const googlesql::ResolvedNode>> PostVisitResolvedExpressionColumn(
      std::unique_ptr<const googlesql::ResolvedExpressionColumn> node) override {
    const auto found = names_.find(absl::AsciiStrToLower(node->name()));
    if (found == names_.end()) {
      return absl::InternalError("GRAPH_EXPAND measure references unknown column " + node->name());
    }
    auto builder = googlesql::ToBuilder(std::move(node)).set_name(found->second.name);
    if (found->second.measure_type != nullptr) {
      builder.set_type(found->second.measure_type);
    }
    return std::move(builder).Build();
  }

 private:
  const std::map<std::string, ResultColumn>& names_;
};

// The GoogleSQL query that expands the graph: its node tables LEFT JOINed from the root along
// the schema relationship graph. Flattened output prefixes each table's properties by its label.
struct ExpansionQuery {
  std::string sql;
  std::vector<GraphExpandColumn> columns;
  std::map<int, MeasureDefinition> measures;
  // The measures' definitions borrow their resolved expressions from these outputs.
  std::vector<std::unique_ptr<const googlesql::AnalyzerOutput>> analyses;
};

absl::StatusOr<ExpansionQuery> BuildExpansionQuery(
    googlesql::Catalog* catalog, const googlesql::AnalyzerOptions& options,
    googlesql::TypeFactory& type_factory,
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
  if (literals.at(2).is_null()) {
    return Unsupported("with flatten_results => NULL");
  }
  const bool flatten = literals.at(2).bool_value();
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
    auto label = DefaultLabel(*table);
    if (!label.ok()) {
      return label.status();
    }
    labels.emplace(table, *std::move(label));
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
  Children children;
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

  const bool nested = !flatten && children.contains(root);

  // The tables joined from the root, each edge table that is not inlined between its two node
  // tables.
  const auto* ordered = dynamic_cast<const OrderedPropertyGraph*>(graph);
  const auto order =
      ordered == nullptr ? std::vector<std::string>{} : ordered->element_table_order();
  std::vector<Step> steps = {
      {.table = root, .parent = 0, .referenced = {}, .referencing = {}, .with_key = false},
  };
  // BigQuery emits all siblings before proceeding to their descendants. Appending to the list
  // while visiting it produces that breadth-first order and keeps every parent before its child.
  for (std::size_t i = 0; i < steps.size(); ++i) {
    const auto* node = steps.at(i).table->AsNodeTable();
    if (node != nullptr) {
      if (auto status = AddRelated(node, i, children, order, steps); !status.ok()) {
        return status;
      }
    }
  }
  // The columns of each table that joins compare.
  std::vector<std::set<int>> joined(steps.size());
  for (std::size_t i = 1; i < steps.size(); ++i) {
    joined.at(i).insert(steps.at(i).referenced.begin(), steps.at(i).referenced.end());
    joined.at(steps.at(i).parent)
        .insert(steps.at(i).referencing.begin(), steps.at(i).referencing.end());
  }

  // Each table's subquery exposes its properties as p<i> and the columns its joins compare as
  // c<i>, so that property and column names cannot collide.
  std::vector<std::string> outputs;
  std::vector<std::string> from;
  std::vector<std::vector<GraphExpandColumn>> columns(steps.size());
  std::vector<int> presence(steps.size());
  std::map<int, MeasureDefinition> measures;
  std::vector<std::unique_ptr<const googlesql::AnalyzerOutput>> analyses;
  for (std::size_t i = 0; i < steps.size(); ++i) {
    const Step& step = steps.at(i);
    const googlesql::GraphElementTable& element = *step.table;
    const std::string alias = "t" + std::to_string(i);
    std::vector<std::string> projections;
    auto properties = Properties(element);
    if (!properties.ok()) {
      return properties.status();
    }
    if (step.with_key) {
      // BigQuery appends an edge's key columns not already exposed as properties, but not the
      // other columns that reference the previous table.
      for (const int column : element.GetKeyColumns()) {
        const std::string name = ColumnName(element, column);
        const auto same = std::ranges::find_if(
            *properties, [&](const Property& p) { return absl::EqualsIgnoreCase(p.name, name); });
        if (same == properties->end()) {
          properties->push_back({.name = name, .sql = googlesql::ToIdentifierLiteral(name)});
        } else if (!absl::EqualsIgnoreCase(same->sql, name)) {
          return Unsupported("of an edge table with a property named after a key column");
        }
      }
    }
    std::map<std::string, ResolvedMeasure> resolved_measures;
    if (std::ranges::any_of(*properties, &Property::is_measure)) {
      auto resolved = ResolveMeasures(element, options, *catalog, type_factory, analyses);
      if (!resolved.ok()) {
        return resolved.status();
      }
      resolved_measures = *std::move(resolved);
      for (const auto& [name, measure] : resolved_measures) {
        if (auto status = UnrecognizedAggregate(measure, flatten ? labels.at(&element) + "_" : "");
            !status.ok()) {
          return status;
        }
      }
    }
    // The body output of each measure and the projection of each other property, by lowercase
    // name.
    std::map<std::string, int> measure_outputs;
    std::map<std::string, std::size_t> scalar_properties;
    for (std::size_t p = 0; p < properties->size(); ++p) {
      const auto& definition = properties->at(p);
      const std::string& property = properties->at(p).name;
      if (definition.is_measure) {
        measure_outputs.emplace(absl::AsciiStrToLower(property), static_cast<int>(outputs.size()));
      } else {
        projections.push_back("(" + definition.sql + ") AS p" + std::to_string(p));
        scalar_properties.emplace(absl::AsciiStrToLower(property), p);
      }
      const std::string name = flatten ? labels.at(&element) + "_" + property : property;
      columns.at(i).push_back(
          {.name = name, .body_index = static_cast<int>(outputs.size()), .fields = {}});
      const std::string body_name = nested ? "v" + std::to_string(outputs.size()) : name;
      if (definition.is_measure) {
        // The measure rewriter replaces AGG calls with aggregates over captured
        // columns. This slot has no runtime value but keeps the layout indexed.
        outputs.push_back("NULL AS " + googlesql::ToIdentifierLiteral(body_name));
      } else {
        outputs.push_back(alias + ".p" + std::to_string(p) + " AS " +
                          googlesql::ToIdentifierLiteral(body_name));
      }
    }
    auto& join_columns = joined.at(i);
    if (!measure_outputs.empty()) {
      // Measures capture the element's key and the properties they aggregate as pseudo-columns.
      const auto add_pseudo = [&](const std::string& value) {
        const int index = static_cast<int>(outputs.size());
        const std::string name = "$graph_expand_" + std::to_string(index);
        columns.at(i).push_back(
            {.name = name, .body_index = index, .is_pseudo = true, .fields = {}});
        outputs.push_back(value + " AS " + googlesql::ToIdentifierLiteral(name));
        return index;
      };
      std::vector<int> keys;
      for (const int j : element.GetKeyColumns()) {
        join_columns.insert(j);
        keys.push_back(add_pseudo(alias + ".c" + std::to_string(j)));
      }
      std::map<std::string, int> values;
      for (const auto& [name, measure] : resolved_measures) {
        for (const auto& referenced : ReferencedNames(*measure.expr)) {
          if (measure_outputs.contains(referenced) || values.contains(referenced)) {
            continue;
          }
          const auto property = scalar_properties.find(referenced);
          if (property == scalar_properties.end()) {
            return absl::InternalError("GRAPH_EXPAND measure references unknown property " +
                                       referenced);
          }
          values.emplace(referenced, add_pseudo(alias + ".p" + std::to_string(property->second)));
        }
      }
      for (const auto& [name, output] : measure_outputs) {
        const auto& resolved = resolved_measures.at(name);
        MeasureDefinition measure{
            .sql = resolved.sql,
            .expr = resolved.expr,
            .references = {},
            .keys = keys,
        };
        for (const auto& referenced : ReferencedNames(*resolved.expr)) {
          const auto found = measure_outputs.find(referenced);
          measure.references.emplace(
              referenced, found == measure_outputs.end() ? values.at(referenced) : found->second);
        }
        measures.emplace(output, std::move(measure));
      }
    }
    std::vector<std::string> conditions;
    conditions.reserve(step.referenced.size());
    for (std::size_t c = 0; c < step.referenced.size(); ++c) {
      conditions.push_back(alias + ".c" + std::to_string(step.referenced.at(c)) + " = t" +
                           std::to_string(step.parent) + ".c" +
                           std::to_string(step.referencing.at(c)));
    }
    std::ranges::transform(join_columns, std::back_inserter(projections), [&](int column) {
      return googlesql::ToIdentifierLiteral(ColumnName(element, column)) + " AS c" +
             std::to_string(column);
    });
    if (nested && i > 0) {
      projections.emplace_back("1 AS unit");
      presence.at(i) = static_cast<int>(outputs.size());
      outputs.push_back(alias + ".unit AS v" + std::to_string(outputs.size()));
    } else if (projections.empty()) {
      projections.emplace_back("1 AS unit");
    }
    const std::string table = "(SELECT " + absl::StrJoin(projections, ", ") + " FROM " +
                              googlesql::ToIdentifierLiteral(element.GetTable()->FullName()) +
                              ") AS " + alias;
    from.push_back(i == 0 ? table
                          : "LEFT JOIN " + table + " ON " + absl::StrJoin(conditions, " AND "));
  }
  if (std::ranges::all_of(columns, [](const auto& fields) { return fields.empty(); })) {
    return absl::InvalidArgumentError("GRAPH_EXPAND requires a property on an expanded table");
  }
  if (nested) {
    for (std::size_t i = steps.size() - 1; i > 0; --i) {
      // Prepend siblings as we walk backward so the nested layout follows declaration order.
      const Step& step = steps.at(i);
      auto& parent = columns.at(step.parent);
      const std::string& name = labels.at(step.table);
      if (std::ranges::any_of(parent, [&](const GraphExpandColumn& column) {
            return absl::EqualsIgnoreCase(column.name, name);
          })) {
        return Unsupported(
            "with flatten_results => FALSE and a property named after a joined table");
      }
      const auto first_row = std::ranges::find_if(parent, &GraphExpandColumn::is_row);
      parent.insert(first_row, GraphExpandColumn{
                                   .name = name,
                                   .body_index = presence.at(i),
                                   .is_row = true,
                                   .fields = std::move(columns.at(i)),
                               });
    }
  } else {
    for (std::size_t i = 1; i < steps.size(); ++i) {
      std::ranges::move(columns.at(i), std::back_inserter(columns.front()));
    }
  }
  return ExpansionQuery{
      .sql = "SELECT " + absl::StrJoin(outputs, ", ") + " FROM " + absl::StrJoin(from, " "),
      .columns = std::move(columns.front()),
      .measures = std::move(measures),
      .analyses = std::move(analyses),
  };
}

struct ResolvedExpansion {
  std::unique_ptr<const googlesql::AnalyzerOutput> body;
  std::vector<GraphExpandColumn> columns;
  // Catalog measure metadata borrows the resolved expressions from these, which are renamed
  // copies of expressions the analyses own.
  std::vector<std::unique_ptr<const googlesql::AnalyzerOutput>> measure_analyses;
  std::vector<std::unique_ptr<const googlesql::ResolvedExpr>> measure_expressions;
  // Each ROW refers to a table describing its fields. Keep those tables with the call, not in
  // the catalog: the schema may differ for two calls to the same TVF in one statement.
  std::vector<std::unique_ptr<googlesql::SimpleTable>> row_tables;
  std::unique_ptr<googlesql::SimpleTable> result_table;
  // The type of each measure's result column, by body index, created before any derived measure
  // refers to it.
  std::map<int, const googlesql::Type*> measure_types;

  [[nodiscard]] const googlesql::ResolvedQueryStmt& query() const {
    return *body->resolved_statement()->GetAs<googlesql::ResolvedQueryStmt>();
  }

  absl::StatusOr<std::unique_ptr<googlesql::SimpleTable>> TableSchema(
      const std::string& name, const std::vector<GraphExpandColumn>& fields,
      const std::map<int, MeasureDefinition>& measures, googlesql::TypeFactory& type_factory) {
    auto table = std::make_unique<googlesql::SimpleTable>(name);
    std::map<int, int> field_indices;
    for (std::size_t i = 0; i < fields.size(); ++i) {
      field_indices.emplace(fields.at(i).body_index, static_cast<int>(i));
    }
    for (const auto& field : fields) {
      auto type = query().output_column_list(field.body_index)->column().annotated_type();
      googlesql::SimpleColumn::Attributes attributes;
      attributes.is_pseudo_column = field.is_pseudo;
      if (field.is_row) {
        auto child = TableSchema(field.name, field.fields, measures, type_factory);
        if (!child.ok()) {
          return child.status();
        }
        const googlesql::RowType* row = nullptr;
        if (auto status = type_factory.MakeRowType(child->get(), field.name, &row); !status.ok()) {
          return status;
        }
        row_tables.push_back(std::move(*child));
        type = googlesql::AnnotatedType(row, nullptr);
      } else if (const auto measure = measures.find(field.body_index); measure != measures.end()) {
        const auto& definition = measure->second;
        // The measure rewriter binds expression columns by name in this result schema.
        std::map<std::string, ResultColumn> names;
        for (const auto& [referenced, index] : definition.references) {
          const auto found = measure_types.find(index);
          names.emplace(referenced,
                        ResultColumn{
                            .name = fields.at(field_indices.at(index)).name,
                            .measure_type = found == measure_types.end() ? nullptr : found->second,
                        });
        }
        auto copy = googlesql::ResolvedASTDeepCopyVisitor::Copy(definition.expr);
        if (!copy.ok()) {
          return copy.status();
        }
        RenameExpressionColumns rename(names);
        auto expr = rename.VisitAll<googlesql::ResolvedExpr>(*std::move(copy));
        if (!expr.ok()) {
          return expr.status();
        }
        type = googlesql::AnnotatedType(measure_types.at(field.body_index), nullptr);
        std::vector<int> keys;
        std::ranges::transform(definition.keys, std::back_inserter(keys),
                               [&](int index) { return field_indices.at(index); });
        auto metadata = googlesql::Column::ExpressionAttributes::Create(
            googlesql::Column::ExpressionAttributes::ExpressionKind::MEASURE_EXPRESSION,
            definition.sql, expr->get(), std::move(keys));
        if (!metadata.ok()) {
          return metadata.status();
        }
        attributes.column_expression = std::move(*metadata);
        measure_expressions.push_back(*std::move(expr));
      }
      if (auto status = table->AddColumn(
              std::make_unique<googlesql::SimpleColumn>(name, field.name, type, attributes));
          !status.ok()) {
        return status;
      }
    }
    return table;
  }
};

class ExpansionSignature : public googlesql::TVFSignature {
 public:
  ExpansionSignature(const std::vector<googlesql::TVFInputArgumentType>& arguments,
                     const googlesql::TVFRelation& relation,
                     std::shared_ptr<const ResolvedExpansion> expansion)
      : TVFSignature(arguments, relation, expansion->result_table.get(), Options()),
        expansion_(std::move(expansion)) {}

  [[nodiscard]] const ResolvedExpansion& expansion() const { return *expansion_; }

 private:
  static googlesql::TVFSignatureOptions Options() {
    googlesql::TVFSignatureOptions options;
    options.row_type_rewrite_callback = RewriteRows;
    return options;
  }

  static absl::StatusOr<std::shared_ptr<googlesql::TVFSignature>> RewriteRows(
      const googlesql::TVFSignature& signature, const googlesql::TVFRelation& relation) {
    return std::make_shared<ExpansionSignature>(signature.input_arguments(), relation,
                                                signature.GetAs<ExpansionSignature>()->expansion_);
  }

  std::shared_ptr<const ResolvedExpansion> expansion_;
};

class GraphExpand : public googlesql::TableValuedFunction {
 public:
  GraphExpand() : TableValuedFunction({"graph_expand"}, "", {Signature()}) {}

  absl::Status Resolve(const googlesql::AnalyzerOptions* analyzer_options,
                       const std::vector<googlesql::TVFInputArgumentType>& input_arguments,
                       const googlesql::FunctionSignature& /*concrete_signature*/,
                       googlesql::Catalog* catalog, googlesql::TypeFactory* type_factory,
                       std::shared_ptr<googlesql::TVFSignature>* tvf_signature) const final {
    auto query = BuildExpansionQuery(catalog, *analyzer_options, *type_factory, input_arguments);
    if (!query.ok()) {
      return query.status();
    }
    auto expansion = std::make_shared<ResolvedExpansion>();
    if (auto status = googlesql::AnalyzeStatement(query->sql, *analyzer_options, catalog,
                                                  type_factory, &expansion->body);
        !status.ok()) {
      return status;
    }
    expansion->columns = std::move(query->columns);
    expansion->measure_analyses = std::move(query->analyses);
    for (const auto& [index, measure] : query->measures) {
      auto type = type_factory->MakeMeasureType(measure.expr->type());
      if (!type.ok()) {
        return type.status();
      }
      expansion->measure_types.emplace(index, *type);
    }
    auto result =
        expansion->TableSchema("graph_expand", expansion->columns, query->measures, *type_factory);
    if (!result.ok()) {
      return result.status();
    }
    std::vector<googlesql::TVFSchemaColumn> schema;
    for (int i = 0; i < (*result)->NumColumns(); ++i) {
      const auto* column = (*result)->GetColumn(i);
      schema.emplace_back(
          column->Name(),
          googlesql::AnnotatedType(column->GetType(), column->GetTypeAnnotationMap()),
          column->IsPseudoColumn());
    }
    if (!query->measures.empty()) {
      expansion->result_table = std::move(*result);
    }
    *tvf_signature = std::make_shared<ExpansionSignature>(
        input_arguments, googlesql::TVFRelation(std::move(schema)), std::move(expansion));
    return absl::OkStatus();
  }
};

}  // namespace

std::optional<GraphExpandBody> GetGraphExpandBody(const googlesql::TVFSignature& signature) {
  if (!signature.Is<ExpansionSignature>()) {
    return std::nullopt;
  }
  const auto& expansion = signature.GetAs<ExpansionSignature>()->expansion();
  return GraphExpandBody{.query = expansion.query(), .columns = expansion.columns};
}

void AddGraphExpand(googlesql::SimpleCatalog& catalog) {
  catalog.AddOwnedTableValuedFunction(std::make_unique<GraphExpand>());
}

}  // namespace bigquery_emulator_duckdb::analyzer_functions
