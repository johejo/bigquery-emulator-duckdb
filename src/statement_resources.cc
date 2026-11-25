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
#include "src/key_ranges.h"
#include "src/references.h"

namespace bigquery_emulator_duckdb {
namespace {

// BigQuery reports at most this many warnings of a job.
constexpr size_t kMaxWarnings = 100;

}  // namespace

std::optional<std::string> StatementResources::ReadQuery(
    const TableReference& table, const std::optional<TableRead>& definition,
    StatementResources* resources, const std::optional<std::set<std::string>>& columns,
    const ReadFields& fields, const std::optional<KeyRanges>& keys) {
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
  const auto* projected = std::get_if<FieldProjectedRead>(&*definition);
  const ReadFields no_fields;
  const auto& selected_fields = projected != nullptr ? fields : no_fields;
  for (const auto& [column, ordinals] : selected_fields) {
    key.append(2, '\0');
    key += column;
    for (const size_t ordinal : ordinals) {
      key += '\0';
      key += std::to_string(ordinal);
    }
  }
  const std::optional<KeyRanges> no_keys;
  const auto& selected_keys =
      projected != nullptr && !projected->key_column.empty() ? keys : no_keys;
  if (selected_keys) {
    // Length prefixes keep keys, which can hold any bytes, apart.
    key.append(3, '\0');
    for (const auto& range : *selected_keys) {
      for (const auto& bound : {range.start, range.end}) {
        key += bound ? (bound->inclusive ? '[' : '(') + std::to_string(bound->key.size()) + ':' +
                           bound->key
                     : "*";
      }
    }
  }
  if (const auto found = resources->reads_.find(key); found != resources->reads_.end()) {
    return found->second;
  }
  TableReadContext context{
      .files = resources->files_,
      .columns = columns,
      .fields = selected_fields,
      .keys = selected_keys,
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
  const auto& prepare =
      projected != nullptr ? projected->prepare : std::get<PrepareTableRead>(*definition);
  std::string query = prepare(context);
  resources->reads_.emplace(key, query);
  return query;
}

}  // namespace bigquery_emulator_duckdb
