#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "src/api_error.h"
#include "src/references.h"
#include "src/temporary_files.h"

namespace bigquery_emulator_duckdb {

// Runs a DuckDB query and returns its rows, each value as text or NULL.
using TextQuery =
    std::function<std::vector<std::vector<std::optional<std::string>>>(const std::string& sql)>;

// What preparing a read of a table has besides its definition.
struct TableReadContext {
  // The owner of the read's temporary files, including those written before it throws.
  TemporaryFiles& files;
  // The names of the table's columns the statement reads, or none for all of them.
  const std::optional<std::set<std::string>>& columns;
  const TextQuery& query;
  // Adds a warning to the job, such as of a bad record the read skips.
  std::function<void(const ApiError& warning)> warn;
  // Return a schema-only query without acquiring sources; binding can otherwise open files.
  bool dry_run = false;
};

// A read definition, resolved without acquiring its source. A fixed query needs no staging.
// A preparation owns its definition by value: it must not borrow the analyzer, catalog or
// TableSource.
using TableRead = std::variant<std::string, std::function<std::string(TableReadContext&)>>;

// One statement's preparation cache and staged files. Never shared between statements or jobs;
// shared ownership only extends this statement's lifetime through consumption of its results.
// Preparation is single-threaded, just as translation of one statement is.
class StatementResources {
 public:
  enum class Mode : uint8_t { kExecute, kDryRun };

  // `query` runs the queries that preparations need.
  explicit StatementResources(Mode mode, TextQuery query = {})
      : mode_(mode), query_(std::move(query)) {}
  ~StatementResources() = default;
  StatementResources(const StatementResources&) = delete;
  StatementResources& operator=(const StatementResources&) = delete;

  // Prepares a resolved table at most once for each set of `columns` it reads, none for all of
  // them. Fixed queries also work without an execution owner; resource-backed reads without one
  // are rejected before accessing the source.
  static std::optional<std::string> ReadQuery(
      const TableReference& table, const std::optional<TableRead>& definition,
      StatementResources* resources,
      const std::optional<std::set<std::string>>& columns = std::nullopt);

  // The warnings of the statement's reads, in the order they were added, each once, and at most
  // the first 100, as BigQuery reports them.
  [[nodiscard]] const std::vector<ApiError>& warnings() const { return warnings_; }

 private:
  Mode mode_;
  TextQuery query_;
  TemporaryFiles files_;
  std::map<std::string, std::string> reads_;
  std::vector<ApiError> warnings_;
};

}  // namespace bigquery_emulator_duckdb
