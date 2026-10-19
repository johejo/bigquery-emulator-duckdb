#pragma once

#include <optional>
#include <set>
#include <string>
#include <vector>

#include "absl/types/span.h"

namespace bigquery_emulator_duckdb {

class TableSource;

// Splits a table path into its dot-separated parts. An element with dots in it, which is how
// `project.dataset.table` in backquotes arrives, is split too; dots before the colon in a
// domain-scoped project ID are kept in the project.
std::vector<std::string> SplitTablePath(absl::Span<const std::string> path);

// Normalizes a table path to {project, dataset, table}, splitting it with SplitTablePath first.
// Missing parts come from the defaults, and a project number becomes the ID of the project
// `source` holds by it, when `source` is not null. Returns an empty vector for a path it cannot
// interpret.
std::vector<std::string> NormalizeTablePath(absl::Span<const std::string> path,
                                            const std::string& default_project,
                                            const std::string& default_dataset,
                                            const TableSource* source);

// The dataset qualifier that names a temporary table explicitly, as `_SESSION.name`.
inline constexpr char kSessionDataset[] = "_SESSION";

// The temporary tables of a multi-statement query, which live in the DuckDB catalog `project`
// and its schema `dataset`: those named `names` when a statement is analyzed and translated.
struct TemporaryTables {
  std::string project;
  std::string dataset;
  std::set<std::string> names = {};
};

// The name of the temporary table that CREATE TEMP TABLE `path` creates, which is either `name`
// or `_SESSION.name`, or nothing for any other path.
std::optional<std::string> TemporaryTableName(absl::Span<const std::string> path);

// Normalizes a table path as NormalizeTablePath does, in a multi-statement query whose temporary
// tables are `temporary` when it is not null: `_SESSION.name` names a temporary table, and so
// does an unqualified name when a temporary table has it.
std::vector<std::string> ResolveTablePath(absl::Span<const std::string> path,
                                          const std::string& default_project,
                                          const std::string& default_dataset,
                                          const TemporaryTables* temporary,
                                          const TableSource* source);

}  // namespace bigquery_emulator_duckdb
