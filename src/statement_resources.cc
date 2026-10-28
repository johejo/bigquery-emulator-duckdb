#include "src/statement_resources.h"

#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <variant>

#include "src/references.h"
#include "src/temporary_files.h"

namespace bigquery_emulator_duckdb {

std::optional<std::string> StatementResources::ReadQuery(const TableReference& table,
                                                         const std::optional<TableRead>& definition,
                                                         StatementResources* resources) {
  if (!definition) {
    return std::nullopt;
  }
  if (const auto* query = std::get_if<std::string>(&*definition)) {
    return *query;
  }
  if (resources == nullptr) {
    throw std::invalid_argument(
        "Resource-backed table reads are unsupported in stored definitions or without an "
        "execution resource owner");
  }
  if (resources->mode_ == Mode::kDryRun) {
    // DuckDB can open files while binding a reader, even without executing it. Until external
    // dry-run semantics are specified, do not prepare sources or send their readers to Prepare.
    throw std::invalid_argument("Dry runs of resource-backed table reads are not supported");
  }
  const std::string key = QualifiedName(table);
  if (const auto found = resources->reads_.find(key); found != resources->reads_.end()) {
    return found->second;
  }
  std::string query =
      std::get<std::function<std::string(TemporaryFiles&)>>(*definition)(resources->files_);
  resources->reads_.emplace(key, query);
  return query;
}

}  // namespace bigquery_emulator_duckdb
