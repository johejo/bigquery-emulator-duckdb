#include "src/emulator.h"

#include <cctype>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <ios>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "nlohmann/json.hpp"
#include "src/api_error.h"
#include "src/backend_error.h"
#include "src/backend_initializer.h"
#include "src/builtin_function_options.h"
#include "src/duckdb_catalog.h"
#include "src/duckdb_sql.h"
#include "src/external_query.h"
#include "src/googlesql_functions_duckdb/options.h"
#include "src/googlesql_geography/options.h"
#include "src/object_ref.h"
#include "src/project.h"
#include "src/table_history.h"
#include "src/table_metadata.h"

namespace bigquery_emulator_duckdb {
namespace {

using nlohmann::json;

// The file a project is stored in. Project ids may carry a domain ("example.com:project"), so
// every byte outside [A-Za-z0-9_-] is percent-encoded. With '.' and '/' encoded, no id can name a
// path outside the data directory.
std::string ProjectFileName(const std::string& project_id) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  std::string name;
  for (const char c : project_id) {
    const auto byte = static_cast<unsigned char>(c);
    if (std::isalnum(byte) != 0 || c == '_' || c == '-') {
      name += c;
    } else {
      name += '%';
      name += kHex[byte >> 4U];
      name += kHex[byte & 0xFU];
    }
  }
  return name + ".duckdb";
}

}  // namespace

Emulator::Emulator(std::string data_dir, const std::vector<Project>& projects,
                   const std::optional<std::string>& session_user,
                   const BackendInitializer& initialize,
                   const std::vector<ExternalConnection>& connections,
                   const googlesql_geography::Options& geography,
                   const functions_duckdb::Options& functions,
                   ExcludedFunctionIds excluded_function_ids, FunctionDeclarations declarations)
    : backend_(
          session_user,
          {
              .fetch =
                  [this](const std::string& objectref, const std::string& project) {
                    return FetchObjectMetadata(gcs_client_, connections_.conninfo, objectref,
                                               project);
                  },
              .has_connection =
                  [this](const std::string& key) { return connections_.conninfo.contains(key); },
          },
          geography, functions, initialize),
      excluded_function_ids_(std::move(excluded_function_ids)),
      declarations_(declarations),
      has_session_user_(session_user.has_value()),
      data_dir_(std::move(data_dir)) {
  for (const ExternalConnection& connection : connections) {
    // cppcheck-suppress useStlAlgorithm
    if (!connections_.conninfo.emplace(connection.key, connection.conninfo).second) {
      throw std::invalid_argument("Duplicate connection: " + connection.key);
    }
  }
  const auto registry = std::filesystem::path(data_dir_) / "projects.json";
  std::map<std::string, Project> registered;
  if (!data_dir_.empty()) {
    std::filesystem::create_directories(data_dir_);
    if (std::filesystem::exists(registry)) {
      std::ifstream input(registry);
      if (!input) {
        throw std::runtime_error("Cannot read " + registry.string());
      }
      const json saved = json::parse(input);
      if (!saved.is_array()) {
        throw std::invalid_argument("Project registry must be an array");
      }
      for (const auto& value : saved) {
        const Project project = ParseProject(value);
        if (!registered.emplace(project.project_id, project).second) {
          throw std::invalid_argument("Duplicate saved projectId: " + project.project_id);
        }
      }
    }
  }
  std::set<std::string> supplied;
  for (const Project& project : projects) {
    // Validate callers of the C++ boundary as well as CLI JSON.
    ParseProject(ProjectJson(project));
    if (!supplied.insert(project.project_id).second) {
      throw std::invalid_argument("Duplicate projectId: " + project.project_id);
    }
    registered.insert_or_assign(project.project_id, project);
  }
  for (const auto& [id, project] : registered) {
    project_ids_.emplace(id, id);
    projects_.push_back(project);
  }
  for (const Project& project : projects_) {
    if (!project.numeric_id) {
      continue;
    }
    const auto alias = project_ids_.emplace(*project.numeric_id, project.project_id).first;
    if (alias->second != project.project_id) {
      throw std::invalid_argument("Conflicting numericId: " + *project.numeric_id);
    }
  }
  for (const Project& project : projects_) {
    const std::string database = ProjectDatabase(project.project_id);
    try {
      backend_.Execute("ATTACH " + QuoteLiteral(database) + " AS " +
                       QuoteIdentifier(project.project_id));
      backend_.Execute("CREATE TABLE IF NOT EXISTS " + DatasetMetadataTable(project.project_id) +
                       " (dataset_id VARCHAR, metadata VARCHAR)");
      backend_.Execute(
          "CREATE TABLE IF NOT EXISTS " + PropertyGraphsTable(project.project_id) +
          " (dataset_id VARCHAR, graph_id VARCHAR, definition VARCHAR, native_sql VARCHAR)");
      for (const std::string& statement : CreateHistoryStatements(project.project_id)) {
        backend_.Execute(statement);
      }
    } catch (const BackendError& error) {
      throw ApiError::Internal("Failed to open " + database + ": " + error.what());
    }
  }
  // All projects must be attached before graph definitions can refer to their tables.
  for (const Project& project : projects_) {
    const auto graphs =
        backend_.Execute("SELECT native_sql FROM " + PropertyGraphsTable(project.project_id));
    for (size_t row = 0; row < graphs.rows.size(); ++row) {
      try {
        backend_.Execute(graphs.rows.String(row, 0));
      } catch (const BackendError&) {
        // A graph whose input table was dropped or altered must fail when queried,
        // without preventing the emulator from opening its other datasets.
        continue;
      }
    }
  }
  if (!data_dir_.empty()) {
    json saved = json::array();
    for (const Project& project : projects_) {
      saved.push_back(ProjectJson(project));
    }
    const auto temporary = registry.string() + ".tmp";
    {
      std::ofstream output(temporary);
      output.exceptions(std::ios::failbit | std::ios::badbit);
      output << saved.dump(2) << '\n';
      output.close();
    }
    std::filesystem::rename(temporary, registry);
  }
}

const std::vector<Project>& Emulator::ListProjects() const { return projects_; }

std::string Emulator::ResolveProject(const std::string& project_id) const {
  const auto found = project_ids_.find(project_id);
  if (found == project_ids_.end()) {
    throw ApiError::NotFound("Not found: Project " + project_id);
  }
  return found->second;
}

std::string Emulator::ProjectDatabase(const std::string& project_id) const {
  if (data_dir_.empty()) {
    return ":memory:";
  }
  return (std::filesystem::path(data_dir_) / ProjectFileName(project_id)).string();
}

}  // namespace bigquery_emulator_duckdb
