#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "src/backend.h"
#include "src/query_parameters.h"

namespace googlesql {
class ParserOutput;
}

namespace bigquery_emulator_duckdb {

class TableSource;
struct TranslatedStatement;

// What a multi-statement query needs from the emulator that runs it.
class ScriptHost {
 public:
  ScriptHost() = default;
  ScriptHost(const ScriptHost&) = delete;
  ScriptHost& operator=(const ScriptHost&) = delete;
  virtual ~ScriptHost() = default;

  // The backend that the script's temporary tables are attached to and its sessions come from.
  virtual Backend& backend() = 0;
  // Whether SESSION_USER is enabled.
  [[nodiscard]] virtual bool has_session_user() const = 0;
  // A catalog source for the tables, views and datasets the emulator holds, as `session` sees
  // them.
  virtual std::unique_ptr<TableSource> NewTableSource(Backend* session, bool expire) = 0;
  // Runs `translation` in `session` as a query job without a destination table does, and returns
  // its result with the schema the translation gives it.
  virtual QueryResult RunStatement(const TranslatedStatement& translation,
                                   const std::vector<std::string>& setup, bool null_arrays,
                                   Backend* session) = 0;
};

// Runs the multi-statement query `script`, the text `query`, with `setup` selecting the default
// dataset `default_project`.`default_dataset`, and returns the result of the last statement it
// ran. `query_project` is the executing project, used when @@dataset_project_id is NULL.
// `number` names the database of its temporary tables, so each script running at once takes
// its own. Rejects parameters and statements the emulator does not run in scripts.
QueryResult RunScript(ScriptHost& host, const std::string& query,
                      const googlesql::ParserOutput& script, const QueryParameters& parameters,
                      const std::string& default_project, const std::string& default_dataset,
                      const std::vector<std::string>& setup, const std::string& query_project,
                      int64_t number);

}  // namespace bigquery_emulator_duckdb
