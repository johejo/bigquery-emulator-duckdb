#include "src/statement_resources.h"

#include <algorithm>
#include <cstddef>
#include <functional>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>

#include "src/api_error.h"
#include "src/references.h"

namespace bigquery_emulator_duckdb {
namespace {

// BigQuery reports at most this many warnings of a job.
constexpr size_t kMaxWarnings = 100;

}  // namespace

std::optional<std::string> StatementResources::ReadQuery(
    const TableReference& table, const std::optional<TableRead>& definition,
    StatementResources* resources, const std::optional<std::set<std::string>>& columns) {
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
  // No column name holds a NUL or is *.
  std::string key = QualifiedName(table);
  for (const std::string& column : columns.value_or(std::set<std::string>{"*"})) {
    key += '\0';
    key += column;
  }
  if (const auto found = resources->reads_.find(key); found != resources->reads_.end()) {
    return found->second;
  }
  TableReadContext context{
      .files = resources->files_,
      .columns = columns,
      .query = resources->query_,
      .warn =
          [resources](const ApiError& warning) {
            // A record that reads of different columns both skip is warned of once.
            if (resources->warnings_.size() < kMaxWarnings &&
                std::ranges::none_of(resources->warnings_, [&](const ApiError& added) {
                  return added.location() == warning.location() &&
                         std::string_view(added.what()) == warning.what();
                })) {
              resources->warnings_.push_back(warning);
            }
          },
      .dry_run = resources->mode_ == Mode::kDryRun,
  };
  std::string query = std::get<std::function<std::string(TableReadContext&)>>(*definition)(context);
  resources->reads_.emplace(key, query);
  return query;
}

}  // namespace bigquery_emulator_duckdb
