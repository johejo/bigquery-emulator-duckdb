#include <algorithm>
#include <cstddef>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "googlesql/public/type.h"
#include "googlesql/resolved_ast/resolved_ast.h"
#include "src/translator/context.h"
#include "src/translator/statement.h"

namespace bigquery_emulator_duckdb::translator {
namespace {

// A table scan lists top-level columns only. Look at their uses before preparing reads, since
// reading a whole Bigtable family instead of a named qualifier can change which rows survive.
class FieldUses {
 public:
  std::map<int, std::set<size_t>> Collect(const googlesql::ResolvedNode& node) {
    Visit(node);
    for (const auto* ref : with_refs_) {
      const auto entry = with_entries_.find(ref->with_query_name());
      if (entry != with_entries_.end() &&
          ref->column_list_size() == entry->second->column_list_size()) {
        Link(ref->column_list(), entry->second->column_list());
      }
    }
    std::map<int, std::set<size_t>> result;
    while (!uses_.empty()) {
      const auto [id, field] = uses_.back();
      uses_.pop_back();
      auto [entry, inserted] = result.try_emplace(id);
      auto& fields = entry->second;
      if ((!inserted && fields.empty()) || (field && !fields.insert(*field).second)) {
        continue;
      }
      if (!field) {
        fields.clear();  // A whole-column read takes precedence over individual fields.
      }
      if (const auto inputs = inputs_.find(id); inputs != inputs_.end()) {
        std::ranges::transform(inputs->second, std::back_inserter(uses_),
                               [field](int input) { return std::pair{input, field}; });
      }
    }
    std::erase_if(result, [](const auto& entry) { return entry.second.empty(); });
    return result;
  }

 private:
  void Use(const googlesql::ResolvedColumn& column, std::optional<size_t> field = std::nullopt) {
    if (column.IsInitialized() && column.type()->IsStruct()) {
      uses_.emplace_back(column.column_id(), field);
    }
  }

  void Link(const std::vector<googlesql::ResolvedColumn>& outputs,
            const std::vector<googlesql::ResolvedColumn>& inputs) {
    for (size_t i = 0; i < outputs.size(); ++i) {
      if (outputs.at(i).type()->IsStruct()) {
        inputs_[outputs.at(i).column_id()].push_back(inputs.at(i).column_id());
      }
    }
  }

  void Visit(const googlesql::ResolvedNode& node) {
    if (node.Is<googlesql::ResolvedGetStructField>()) {
      const auto& get = *node.GetAs<googlesql::ResolvedGetStructField>();
      if (get.expr()->Is<googlesql::ResolvedColumnRef>()) {
        Use(get.expr()->GetAs<googlesql::ResolvedColumnRef>()->column(),
            static_cast<size_t>(get.field_idx()));
        return;
      }
    } else if (node.Is<googlesql::ResolvedColumnRef>()) {
      Use(node.GetAs<googlesql::ResolvedColumnRef>()->column());
    } else if (node.Is<googlesql::ResolvedOutputColumn>()) {
      Use(node.GetAs<googlesql::ResolvedOutputColumn>()->column());
    } else if (node.Is<googlesql::ResolvedColumnHolder>()) {
      Use(node.GetAs<googlesql::ResolvedColumnHolder>()->column());
    } else if (node.Is<googlesql::ResolvedProjectScan>()) {
      const auto& project = *node.GetAs<googlesql::ResolvedProjectScan>();
      for (const auto& computed : project.expr_list()) {
        if (computed->expr()->Is<googlesql::ResolvedColumnRef>() &&
            computed->column().type()->IsStruct()) {
          inputs_[computed->column().column_id()].push_back(
              computed->expr()->GetAs<googlesql::ResolvedColumnRef>()->column().column_id());
        } else {
          Visit(*computed);
        }
      }
      Visit(*project.input_scan());
      return;
    } else if (node.Is<googlesql::ResolvedWithEntry>()) {
      const auto& entry = *node.GetAs<googlesql::ResolvedWithEntry>();
      with_entries_.emplace(entry.with_query_name(), entry.with_subquery());
    } else if (node.Is<googlesql::ResolvedWithRefScan>()) {
      with_refs_.push_back(node.GetAs<googlesql::ResolvedWithRefScan>());
    } else if (node.Is<googlesql::ResolvedSetOperationScan>()) {
      const auto& set = *node.GetAs<googlesql::ResolvedSetOperationScan>();
      for (const auto& input : set.input_item_list()) {
        if (set.op_type() == googlesql::ResolvedSetOperationScan::UNION_ALL) {
          Link(set.column_list(), input->output_column_list());
        } else {
          for (const auto& column : input->output_column_list()) {
            Use(column);
          }
        }
      }
    } else if (node.Is<googlesql::ResolvedRecursiveScan>()) {
      const auto& recursive = *node.GetAs<googlesql::ResolvedRecursiveScan>();
      for (const auto* term : {recursive.non_recursive_term(), recursive.recursive_term()}) {
        for (const auto& column : term->output_column_list()) {
          Use(column);
        }
      }
    } else if (node.Is<googlesql::ResolvedInsertStmt>()) {
      for (const auto& column :
           node.GetAs<googlesql::ResolvedInsertStmt>()->query_output_column_list()) {
        Use(column);
      }
    } else if (node.Is<googlesql::ResolvedSubqueryExpr>()) {
      const auto& subquery = *node.GetAs<googlesql::ResolvedSubqueryExpr>();
      if (subquery.subquery_type() != googlesql::ResolvedSubqueryExpr::EXISTS) {
        for (const auto& column : subquery.subquery()->column_list()) {
          Use(column);
        }
      }
      // The parameter list declares correlated references; their uses are in the subquery.
      if (subquery.in_expr() != nullptr) {
        Visit(*subquery.in_expr());
      }
      Visit(*subquery.subquery());
      return;
    }
    std::vector<const googlesql::ResolvedNode*> children;
    node.GetChildNodes(&children);
    for (const auto* child : children) {
      Visit(*child);
    }
  }

  std::vector<std::pair<int, std::optional<size_t>>> uses_;
  std::map<int, std::vector<int>> inputs_;
  std::map<std::string, const googlesql::ResolvedScan*> with_entries_;
  std::vector<const googlesql::ResolvedWithRefScan*> with_refs_;
};

void CollectForNode(const googlesql::ResolvedNode& node, Context& context) {
  if (context.resources != nullptr) {
    context.read_fields = FieldUses{}.Collect(node);
  }
}

}  // namespace

void CollectReadFields(const googlesql::ResolvedStatement& statement, Context& context) {
  CollectForNode(statement, context);
}

void CollectReadFields(const googlesql::ResolvedExpr& expression, Context& context) {
  CollectForNode(expression, context);
}

}  // namespace bigquery_emulator_duckdb::translator
