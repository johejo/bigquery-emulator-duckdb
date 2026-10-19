#include "src/table_path.h"

#include <algorithm>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

#include "absl/strings/str_join.h"
#include "absl/strings/str_split.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "src/table_source.h"

namespace bigquery_emulator_duckdb {

std::vector<std::string> SplitTablePath(absl::Span<const std::string> path) {
  std::vector<std::string> parts;
  for (const std::string& element : path) {
    for (const absl::string_view part : absl::StrSplit(element, '.')) {
      // Each element expands into several owned strings, rather than a direct copy.
      // cppcheck-suppress useStlAlgorithm
      parts.emplace_back(part);
    }
  }
  // Domain-scoped project IDs contain dots before the colon. Those dots belong to the
  // project, even when GoogleSQL passes the entire backtick-quoted path as one element.
  const auto project_part = std::ranges::find_if(
      parts, [](const std::string& part) { return part.find(':') != std::string::npos; });
  if (project_part != parts.end() && project_part != parts.begin()) {
    const auto project_end = std::next(project_part);
    const std::string project = absl::StrJoin(parts.begin(), project_end, ".");
    parts.erase(parts.begin(), project_end);
    parts.insert(parts.begin(), project);
  }
  return parts;
}

std::vector<std::string> NormalizeTablePath(absl::Span<const std::string> path,
                                            const std::string& default_project,
                                            const std::string& default_dataset,
                                            const TableSource* source) {
  std::vector<std::string> parts = SplitTablePath(path);
  if (parts.size() == 1) {
    parts.insert(parts.begin(), {default_project, default_dataset});
  } else if (parts.size() == 2) {
    parts.insert(parts.begin(), default_project);
  }
  if (parts.size() != 3) {
    return {};
  }
  if (std::ranges::any_of(parts, [](const std::string& part) { return part.empty(); })) {
    return {};
  }
  if (source != nullptr) {
    parts.at(0) = source->ProjectId(parts.at(0));
  }
  return parts;
}

std::optional<std::string> TemporaryTableName(absl::Span<const std::string> path) {
  const std::vector<std::string> parts = SplitTablePath(path);
  if (parts.size() == 1 && !parts.at(0).empty()) {
    return parts.at(0);
  }
  if (parts.size() == 2 && parts.at(0) == kSessionDataset && !parts.at(1).empty()) {
    return parts.at(1);
  }
  return std::nullopt;
}

std::vector<std::string> ResolveTablePath(absl::Span<const std::string> path,
                                          const std::string& default_project,
                                          const std::string& default_dataset,
                                          const TemporaryTables* temporary,
                                          const TableSource* source) {
  if (temporary != nullptr) {
    const std::vector<std::string> parts = SplitTablePath(path);
    const bool session = parts.size() == 2 && parts.at(0) == kSessionDataset;
    if (session || (parts.size() == 1 && temporary->names.contains(parts.at(0)))) {
      return {temporary->project, temporary->dataset, parts.back()};
    }
  }
  return NormalizeTablePath(path, default_project, default_dataset, source);
}

}  // namespace bigquery_emulator_duckdb
