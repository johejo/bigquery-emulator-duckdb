#include "src/translator/partition_filter.h"

#include <algorithm>
#include <array>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "googlesql/public/function.h"
#include "googlesql/public/type.h"
#include "googlesql/public/value.h"
#include "googlesql/resolved_ast/resolved_ast.h"
#include "src/api_error.h"
#include "src/catalog_table.h"
#include "src/duckdb_sql.h"
#include "src/language_options.h"
#include "src/table_metadata.h"

namespace bigquery_emulator_duckdb::translator {
namespace {

using Expr = googlesql::ResolvedExpr;
using Scan = googlesql::ResolvedScan;
using Node = googlesql::ResolvedNode;

template <typename T>
const T* As(const Node& node) {
  return node.Is<T>() ? node.GetAs<T>() : nullptr;
}

// Bindings are copied at scan branches: two references to a CTE must each satisfy the
// requirement, even though their definitions share resolved column IDs.
struct Filters {
  std::vector<const Expr*> predicates;
  std::map<int, int> equivalent;
  std::map<std::string, const Scan*> with;

  [[nodiscard]] int Root(int id) const {
    for (;;) {
      const auto it = equivalent.find(id);
      if (it == equivalent.end() || it->second == id) {
        return id;
      }
      id = it->second;
    }
  }
  void Bind(int left, int right) {
    left = Root(left);
    right = Root(right);
    if (left != right) {
      equivalent[left] = right;
    }
  }
};

std::string Name(const googlesql::ResolvedFunctionCall& call) {
  return ToLowerAscii(call.function()->Name());
}

const googlesql::ResolvedColumnRef* Column(const Expr& expr) {
  return As<googlesql::ResolvedColumnRef>(expr);
}

// Pure renames preserve a partition key through projects and derived tables.
void Aliases(const Scan& scan, Filters& filters) {
  if (const auto* project = As<googlesql::ResolvedProjectScan>(scan)) {
    for (const auto& computed : project->expr_list()) {
      if (const auto* ref = Column(*computed->expr())) {
        filters.Bind(computed->column().column_id(), ref->column().column_id());
      }
    }
    Aliases(*project->input_scan(), filters);
  } else if (const auto* filter = As<googlesql::ResolvedFilterScan>(scan)) {
    Aliases(*filter->input_scan(), filters);
  }
}

void Equalities(const Expr& expr, Filters& filters) {
  const auto* call = As<googlesql::ResolvedFunctionCall>(expr);
  if (call == nullptr) {
    return;
  }
  if (Name(*call) == "$and") {
    for (const auto& arg : call->argument_list()) {
      Equalities(*arg, filters);
    }
  } else if (Name(*call) == "$equal" && call->argument_list_size() == 2) {
    const auto* left = Column(*call->argument_list(0));
    const auto* right = Column(*call->argument_list(1));
    if (left != nullptr && right != nullptr) {
      filters.Bind(left->column().column_id(), right->column().column_id());
    }
  }
}

// Source restrictions can constrain an UPDATE/MERGE target through key equality.
// Stop at the same scan boundaries as the ordinary query traversal.
void InputFilters(const Scan& scan, Filters& filters) {
  Aliases(scan, filters);
  if (const auto* project = As<googlesql::ResolvedProjectScan>(scan)) {
    InputFilters(*project->input_scan(), filters);
  } else if (const auto* filter = As<googlesql::ResolvedFilterScan>(scan)) {
    filters.predicates.push_back(filter->filter_expr());
    Equalities(*filter->filter_expr(), filters);
    InputFilters(*filter->input_scan(), filters);
  }
}

// A subquery containing just SELECT <constant> can fold to a constant. A scan or
// aggregate over inline rows is deliberately excluded: BigQuery rejects those bounds.
bool Constant(const Expr& expr) {
  if (expr.Is<googlesql::ResolvedLiteral>() || expr.Is<googlesql::ResolvedParameter>()) {
    return true;
  }
  if (const auto* cast = As<googlesql::ResolvedCast>(expr)) {
    return Constant(*cast->expr()) && (cast->format() == nullptr || Constant(*cast->format())) &&
           (cast->time_zone() == nullptr || Constant(*cast->time_zone()));
  }
  if (const auto* subquery = As<googlesql::ResolvedSubqueryExpr>(expr)) {
    const auto* project = As<googlesql::ResolvedProjectScan>(*subquery->subquery());
    return subquery->subquery_type() == googlesql::ResolvedSubqueryExpr::SCALAR &&
           subquery->parameter_list().empty() && project != nullptr &&
           project->input_scan()->Is<googlesql::ResolvedSingleRowScan>() &&
           project->expr_list_size() == 1 && Constant(*project->expr_list(0)->expr());
  }
  if (const auto* call = As<googlesql::ResolvedFunctionCall>(expr)) {
    return IsBuiltinFunction(*call->function()) &&
           call->function()->function_options().volatility != googlesql::FunctionEnums::VOLATILE &&
           std::ranges::all_of(call->argument_list(),
                               [](const auto& arg) { return Constant(*arg); });
  }
  return false;
}

// Fold Boolean identities before inspecting column references. In particular,
// NOT(FALSE OR FALSE) must never count as eliminating partitions.
std::optional<bool> Boolean(const Expr& expr) {
  if (const auto* literal = As<googlesql::ResolvedLiteral>(expr);
      literal != nullptr && literal->value().type()->IsBool() && !literal->value().is_null()) {
    return literal->value().bool_value();
  }
  const auto* call = As<googlesql::ResolvedFunctionCall>(expr);
  if (call == nullptr) {
    return std::nullopt;
  }
  const std::string name = Name(*call);
  if (name == "$not" && call->argument_list_size() == 1) {
    const auto value = Boolean(*call->argument_list(0));
    return value ? std::optional(!*value) : std::nullopt;
  }
  if (name != "$and" && name != "$or") {
    return std::nullopt;
  }
  bool known = true;
  for (const auto& argument : call->argument_list()) {
    const auto value = Boolean(*argument);
    if (value && *value == (name == "$or")) {
      return value;
    }
    known = known && value.has_value();
  }
  return known ? std::optional(name == "$and") : std::nullopt;
}

// The observed transforms preserve partition elimination for temporal keys. Range
// partitioning does not support arithmetic on its key.
bool PartitionValue(const Expr& expr, int key, const Filters& filters, bool temporal) {
  if (const auto* column = Column(expr)) {
    return filters.Root(column->column().column_id()) == filters.Root(key);
  }
  const auto* call = As<googlesql::ResolvedFunctionCall>(expr);
  if (!temporal || call == nullptr || call->argument_list_size() == 0) {
    return false;
  }
  const std::string name = Name(*call);
  static constexpr auto transforms = std::to_array<std::string_view>({
      "date_add",
      "date_sub",
      "date_trunc",
      "$extract",
      "timestamp_trunc",
      "date",
  });
  // A periodic extraction, such as DAYOFWEEK, is not established by the YEAR case.
  if (name == "$extract") {
    const auto* part = call->argument_list_size() == 2
                           ? As<googlesql::ResolvedLiteral>(*call->argument_list(1))
                           : nullptr;
    if (part == nullptr || part->value().is_null() || !part->value().type()->IsEnum() ||
        part->value().EnumDisplayName() != "YEAR") {
      return false;
    }
  }
  if (!IsBuiltinFunction(*call->function()) ||
      std::ranges::find(transforms, name) == transforms.end() ||
      (name == "date" && call->argument_list_size() != 1) ||
      !PartitionValue(*call->argument_list(0), key, filters, temporal)) {
    return false;
  }
  return std::ranges::all_of(call->argument_list().begin() + 1, call->argument_list().end(),
                             [](const auto& arg) { return Constant(*arg); });
}

// OR requires a predicate entirely on the partition key: BigQuery rejects
// (key = c1 AND id = 1) OR (key = c2 AND id = 2), despite each branch restricting it.
bool Eliminates(const Expr& expr, int key, const Filters& filters, bool temporal,
                bool partition_only = false) {
  if (const auto boolean = Boolean(expr)) {
    return !*boolean;
  }
  const auto* call = As<googlesql::ResolvedFunctionCall>(expr);
  if (call == nullptr) {
    return false;
  }
  const std::string name = Name(*call);
  if (name == "$and" || name == "$or") {
    const auto predicate = [&](const auto& arg) {
      if (const auto boolean = Boolean(*arg)) {
        return name == "$and" ? *boolean : !*boolean;
      }
      return Eliminates(*arg, key, filters, temporal, partition_only || name == "$or");
    };
    if (name == "$or" || partition_only) {
      return std::ranges::all_of(call->argument_list(), predicate);
    }
    return std::ranges::any_of(call->argument_list(), [&](const auto& arg) {
      return Eliminates(*arg, key, filters, temporal);
    });
  }
  if (name == "$not" && call->argument_list_size() == 1) {
    const Expr& argument = *call->argument_list(0);
    return !Boolean(argument).has_value() && Eliminates(argument, key, filters, temporal, true);
  }
  static constexpr auto comparisons = std::to_array<std::string_view>({
      "$equal",
      "$not_equal",
      "$less",
      "$less_or_equal",
      "$greater",
      "$greater_or_equal",
      "$is_null",
      "$between",
      "$in",
  });
  if (std::ranges::find(comparisons, name) == comparisons.end()) {
    return false;
  }
  bool partition = false;
  for (const auto& arg : call->argument_list()) {
    if (PartitionValue(*arg, key, filters, temporal)) {
      if (partition) {
        return false;
      }
      partition = true;
    } else if (!Constant(*arg)) {
      return false;
    }
  }
  return partition;
}

// A Hive-partitioned table's filter eligible for partition elimination: a conjunct that
// references partition keys, `keys` as roots, and no other column, as in k + 1 = 2.
bool EliminatesHive(const Expr& expr, const std::set<int>& keys, const Filters& filters) {
  if (const auto boolean = Boolean(expr)) {
    return !*boolean;
  }
  if (const auto* call = As<googlesql::ResolvedFunctionCall>(expr);
      call != nullptr && Name(*call) == "$and") {
    return std::ranges::any_of(call->argument_list(), [&](const auto& arg) {
      return EliminatesHive(*arg, keys, filters);
    });
  }
  std::vector<const Node*> references;
  expr.GetDescendantsWithKinds({googlesql::RESOLVED_COLUMN_REF}, &references);
  return !references.empty() && std::ranges::all_of(references, [&](const Node* node) {
    return keys.contains(
        filters.Root(node->GetAs<googlesql::ResolvedColumnRef>()->column().column_id()));
  });
}

void Check(const Node& node, Filters filters = {});

void Children(const Node& node, const Filters& filters = {}) {
  std::vector<const Node*> children;
  node.GetChildNodes(&children);
  for (const Node* child : children) {
    Check(*child, filters);
  }
}

void CheckTable(const googlesql::ResolvedTableScan& scan, const Filters& filters) {
  const auto* table = dynamic_cast<const BigQueryTable*>(scan.table());
  const auto description = table == nullptr ? std::nullopt : table->Describe();
  const auto name = [table] {
    return table->reference().project_id + "." + table->reference().dataset_id + "." +
           table->reference().table_id;
  };
  const HivePartitioning* hive = nullptr;
  if (description && description->metadata.external_data &&
      description->metadata.external_data->hive_partitioning) {
    hive = &description->metadata.external_data.value().hive_partitioning.value();
  }
  if (hive != nullptr && !hive->mode.empty() && hive->require_partition_filter.value_or(false)) {
    const auto& fields = hive->fields;
    std::set<int> keys;
    for (int i = 0; i < scan.column_list_size(); ++i) {
      if (std::ranges::find(fields, scan.table()->GetColumn(scan.column_index_list(i))->Name()) !=
          fields.end()) {
        keys.insert(filters.Root(scan.column_list(i).column_id()));
      }
    }
    if (std::ranges::none_of(filters.predicates, [&](const Expr* predicate) {
          return EliminatesHive(*predicate, keys, filters);
        })) {
      throw ApiError::InvalidQuery("Cannot query over table '" + name() +
                                   "' without a filter over column(s) '" + Join(fields, ", ") +
                                   "' that can be used for partition elimination");
    }
    return;
  }
  if (!description || !description->metadata.require_partition_filter) {
    return;
  }
  const auto& metadata = description->metadata;
  const std::string field = metadata.time_partitioning    ? metadata.time_partitioning->field
                            : metadata.range_partitioning ? metadata.range_partitioning->field
                                                          : "";
  for (int i = 0; i < scan.column_list_size(); ++i) {
    if (ToLowerAscii(scan.table()->GetColumn(scan.column_index_list(i))->Name()) ==
        ToLowerAscii(field)) {
      const int key = scan.column_list(i).column_id();
      if (std::ranges::any_of(filters.predicates, [&](const Expr* predicate) {
            return Eliminates(*predicate, key, filters, metadata.time_partitioning.has_value());
          })) {
        return;
      }
    }
  }
  // WHERE FALSE eliminates even a scan whose partition key was pruned from column_list.
  if (std::ranges::any_of(filters.predicates,
                          [](const Expr* expr) { return Boolean(*expr) == false; })) {
    return;
  }
  throw ApiError::InvalidQuery("Cannot query over table '" + name() +
                               "' without a filter over column(s) '" + field +
                               "' that can be used for partition elimination");
}

void Check(const Node& node, Filters filters) {
  if (const auto* table = As<googlesql::ResolvedTableScan>(node)) {
    CheckTable(*table, filters);
  } else if (const auto* filter = As<googlesql::ResolvedFilterScan>(node)) {
    filters.predicates.push_back(filter->filter_expr());
    Check(*filter->input_scan(), filters);
    Children(*filter->filter_expr(),
             Filters{.predicates = {}, .equivalent = {}, .with = filters.with});
  } else if (const auto* project = As<googlesql::ResolvedProjectScan>(node)) {
    Aliases(*project, filters);
    Check(*project->input_scan(), filters);
    for (const auto& expr : project->expr_list()) {
      Children(*expr, Filters{.predicates = {}, .equivalent = {}, .with = filters.with});
    }
  } else if (const auto* with = As<googlesql::ResolvedWithScan>(node)) {
    for (const auto& entry : with->with_entry_list()) {
      filters.with[entry->with_query_name()] = entry->with_subquery();
    }
    Check(*with->query(), filters);
  } else if (const auto* ref = As<googlesql::ResolvedWithRefScan>(node)) {
    const auto it = filters.with.find(ref->with_query_name());
    if (it == filters.with.end()) {
      return;
    }
    const Scan& definition = *it->second;
    for (int i = 0; i < ref->column_list_size(); ++i) {
      filters.Bind(ref->column_list(i).column_id(), definition.column_list(i).column_id());
    }
    // Recursive references are checked by their own scan, rather than expanded forever.
    filters.with.erase(ref->with_query_name());
    Check(definition, filters);
  } else if (const auto* join = As<googlesql::ResolvedJoinScan>(node)) {
    if (join->join_type() == googlesql::ResolvedJoinScan::INNER && join->join_expr() != nullptr) {
      Aliases(*join->left_scan(), filters);
      Aliases(*join->right_scan(), filters);
      Equalities(*join->join_expr(), filters);
      filters.predicates.push_back(join->join_expr());
    }
    Check(*join->left_scan(), filters);
    Check(*join->right_scan(), filters);
    if (join->join_expr() != nullptr) {
      Children(*join->join_expr(),
               Filters{.predicates = {}, .equivalent = {}, .with = filters.with});
    }
  } else {
    // Aggregation, LIMIT and other operators form a boundary for outer predicates.
    // Definitions are checked when read, so CREATE VIEW needs no filter itself.
    Children(node, Filters{.predicates = {}, .equivalent = {}, .with = filters.with});
  }
}

void CheckMerge(const googlesql::ResolvedMergeStmt& merge) {
  const bool never_matches = Boolean(*merge.merge_expr()) == false;
  for (const auto& branch : merge.when_clause_list()) {
    const auto kind = branch->match_type();
    Filters source;
    InputFilters(*merge.from_scan(), source);
    Filters matched = source;
    matched.predicates.push_back(merge.merge_expr());
    Equalities(*merge.merge_expr(), matched);
    if (branch->match_expr() != nullptr) {
      source.predicates.push_back(branch->match_expr());
      matched.predicates.push_back(branch->match_expr());
    }
    if (kind == googlesql::ResolvedMergeWhen::MATCHED) {
      if (!never_matches) {
        CheckTable(*merge.table_scan(), matched);
        Check(*merge.from_scan(), matched);
      }
    } else if (kind == googlesql::ResolvedMergeWhen::NOT_MATCHED_BY_SOURCE) {
      // ON cannot restrict target rows preserved by the outer join.
      Filters target;
      if (branch->match_expr() != nullptr) {
        target.predicates.push_back(branch->match_expr());
      }
      CheckTable(*merge.table_scan(), target);
      if (!never_matches) {
        Check(*merge.from_scan(), matched);
      }
    } else {
      // Source rows preserved for insertion are restricted by WHEN, not ON.
      if (!never_matches) {
        CheckTable(*merge.table_scan(), matched);
      }
      Check(*merge.from_scan(), source);
    }
    Children(*branch);
  }
  Children(*merge.merge_expr());
}

}  // namespace

void CheckPartitionFilters(const googlesql::ResolvedStatement& statement) {
  if (statement.Is<googlesql::ResolvedCreateViewBase>() ||
      statement.Is<googlesql::ResolvedCreateFunctionStmt>() ||
      statement.Is<googlesql::ResolvedCreateTableFunctionStmt>() ||
      statement.Is<googlesql::ResolvedCreateProcedureStmt>() ||
      statement.Is<googlesql::ResolvedCreateTableStmt>() ||
      statement.Is<googlesql::ResolvedCreateSnapshotTableStmt>()) {
    return;
  }
  if (const auto* update = As<googlesql::ResolvedUpdateStmt>(statement)) {
    Filters filters;
    filters.predicates.push_back(update->where_expr());
    Equalities(*update->where_expr(), filters);
    if (update->from_scan() != nullptr) {
      InputFilters(*update->from_scan(), filters);
    }
    CheckTable(*update->table_scan(), filters);
    if (update->from_scan() != nullptr) {
      Check(*update->from_scan(), filters);
    }
    Children(*update->where_expr());
    for (const auto& item : update->update_item_list()) {
      Children(*item);
    }
    return;
  }
  if (const auto* del = As<googlesql::ResolvedDeleteStmt>(statement)) {
    Filters filters;
    filters.predicates.push_back(del->where_expr());
    CheckTable(*del->table_scan(), filters);
    Children(*del->where_expr());
    if (del->using_scan() != nullptr) {
      Check(*del->using_scan(), filters);
    }
    return;
  }
  if (const auto* merge = As<googlesql::ResolvedMergeStmt>(statement)) {
    CheckMerge(*merge);
    return;
  }
  // TRUNCATE removes all partitions without reading the table.
  if (statement.Is<googlesql::ResolvedTruncateStmt>()) {
    return;
  }
  // INSERT's target is a write, rather than a scan requiring partition elimination.
  if (const auto* insert = As<googlesql::ResolvedInsertStmt>(statement)) {
    if (insert->query() != nullptr) {
      Check(*insert->query());
    }
    for (const auto& row : insert->row_list()) {
      Children(*row);
    }
    return;
  }
  Check(statement);
}

}  // namespace bigquery_emulator_duckdb::translator
