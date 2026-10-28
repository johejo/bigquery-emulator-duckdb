#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <variant>

#include "src/references.h"
#include "src/temporary_files.h"

namespace bigquery_emulator_duckdb {

// A read definition, resolved without acquiring its source. A fixed query needs no staging.
// A preparation owns its definition by value: it must not borrow the analyzer, catalog or
// TableSource. It puts temporary files in the supplied owner, including before throwing.
using TableRead = std::variant<std::string, std::function<std::string(TemporaryFiles&)>>;

// One statement's preparation cache and staged files. Never shared between statements or jobs;
// shared ownership only extends this statement's lifetime through consumption of its results.
// Preparation is single-threaded, just as translation of one statement is.
class StatementResources {
 public:
  enum class Mode : uint8_t { kExecute, kDryRun };

  explicit StatementResources(Mode mode) : mode_(mode) {}
  ~StatementResources() = default;
  StatementResources(const StatementResources&) = delete;
  StatementResources& operator=(const StatementResources&) = delete;

  // Prepares a resolved table at most once. Fixed queries also work without an execution
  // owner; resource-backed reads without one are rejected before accessing the source.
  static std::optional<std::string> ReadQuery(const TableReference& table,
                                              const std::optional<TableRead>& definition,
                                              StatementResources* resources);

 private:
  Mode mode_;
  TemporaryFiles files_;
  std::map<std::string, std::string> reads_;
};

}  // namespace bigquery_emulator_duckdb
