#include "src/emulator.h"

#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "nlohmann/json.hpp"
#include "src/api_error.h"
#include "src/backend.h"
#include "src/backend_error.h"
#include "src/ddl_write.h"
#include "src/duckdb_sql.h"
#include "src/emulator_impl.h"
#include "src/field_schema.h"
#include "src/project.h"
#include "src/references.h"
#include "src/routine.h"
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

Emulator::Impl::Impl(std::string data_dir, const std::vector<Project>& projects,
                     const std::optional<std::string>& session_user)
    : backend_(session_user),
      has_session_user_(session_user.has_value()),
      data_dir_(std::move(data_dir)) {
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
    } catch (const BackendError& error) {
      throw ApiError::Internal("Failed to open " + database + ": " + error.what());
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

const std::vector<Project>& Emulator::Impl::ListProjects() const { return projects_; }

std::string Emulator::Impl::ResolveProject(const std::string& project_id) const {
  const auto found = project_ids_.find(project_id);
  if (found == project_ids_.end()) {
    throw ApiError::NotFound("Not found: Project " + project_id);
  }
  return found->second;
}

std::string Emulator::Impl::ProjectDatabase(const std::string& project_id) const {
  if (data_dir_.empty()) {
    return ":memory:";
  }
  return (std::filesystem::path(data_dir_) / ProjectFileName(project_id)).string();
}

Emulator::Emulator(std::string data_dir, const std::vector<Project>& projects,
                   const std::optional<std::string>& session_user)
    : impl_(std::make_unique<Impl>(std::move(data_dir), projects, session_user)) {}

Emulator::~Emulator() = default;

const std::vector<Project>& Emulator::ListProjects() const { return impl_->ListProjects(); }

std::string Emulator::ResolveProject(const std::string& project_id) const {
  return impl_->ResolveProject(project_id);
}

const std::string& Emulator::storage_endpoint() const { return impl_->storage_endpoint(); }

std::shared_ptr<const Job> Emulator::RunQuery(QueryRequest request) {
  return impl_->RunQuery(std::move(request));
}

std::shared_ptr<const Job> Emulator::RunLoad(LoadRequest request) {
  return impl_->RunLoad(std::move(request));
}

std::shared_ptr<const Job> Emulator::RunCopy(CopyRequest request) {
  return impl_->RunCopy(std::move(request));
}

std::shared_ptr<const Job> Emulator::RunExtract(ExtractRequest request) {
  return impl_->RunExtract(std::move(request));
}

std::shared_ptr<const Job> Emulator::GetJob(std::string project_id, const std::string& job_id) {
  return impl_->GetJob(std::move(project_id), job_id);
}

std::vector<std::shared_ptr<const Job>> Emulator::ListJobs(std::string project_id) {
  return impl_->ListJobs(std::move(project_id));
}

void Emulator::DeleteJob(std::string project_id, const std::string& job_id) {
  impl_->DeleteJob(std::move(project_id), job_id);
}

std::vector<std::string> Emulator::ListDatasets(std::string project_id) {
  return impl_->ListDatasets(std::move(project_id));
}

std::vector<DatasetListEntry> Emulator::ListDatasetEntries(std::string project_id) {
  return impl_->ListDatasetEntries(std::move(project_id));
}

DatasetMetadata Emulator::GetDataset(DatasetReference dataset) {
  return impl_->GetDataset(std::move(dataset));
}

void Emulator::CreateDataset(DatasetReference dataset, const DatasetMetadata& metadata) {
  impl_->CreateDataset(std::move(dataset), metadata);
}

void Emulator::UpdateDataset(DatasetReference dataset, const DatasetMetadata& metadata) {
  impl_->UpdateDataset(std::move(dataset), metadata);
}

void Emulator::DeleteDataset(DatasetReference dataset, bool delete_contents) {
  impl_->DeleteDataset(std::move(dataset), delete_contents);
}

std::vector<std::string> Emulator::ListTables(DatasetReference dataset) {
  return impl_->ListTables(std::move(dataset));
}

std::vector<TableListEntry> Emulator::ListTableEntries(DatasetReference dataset) {
  return impl_->ListTableEntries(std::move(dataset));
}

TableInfo Emulator::GetTable(TableReference table, bool include_row_count, bool expire) {
  return impl_->GetTable(std::move(table), include_row_count, expire);
}

TableInfo Emulator::CreateTable(TableReference table, const std::vector<FieldSchema>& schema,
                                const TableMetadata& metadata) {
  return impl_->CreateTable(std::move(table), schema, metadata);
}

TableInfo Emulator::CreateView(TableReference table, const nlohmann::json& definition,
                               const TableMetadata& metadata) {
  return impl_->CreateView(std::move(table), definition, metadata);
}

TableInfo Emulator::UpdateTable(TableReference table,
                                const std::optional<std::vector<FieldSchema>>& schema,
                                const std::optional<nlohmann::json>& view,
                                const std::optional<TableMetadata>& metadata) {
  return impl_->UpdateTable(std::move(table), schema, view, metadata);
}

void Emulator::DeleteTable(TableReference table) { impl_->DeleteTable(std::move(table)); }

std::vector<Routine> Emulator::ListRoutines(DatasetReference dataset) {
  return impl_->ListRoutines(std::move(dataset));
}

Routine Emulator::GetRoutine(RoutineReference routine) {
  return impl_->GetRoutine(std::move(routine));
}

Routine Emulator::WriteRoutine(Routine routine, bool update) {
  return impl_->WriteRoutine(std::move(routine), update);
}

void Emulator::DeleteRoutine(RoutineReference routine) { impl_->DeleteRoutine(std::move(routine)); }

QueryResult Emulator::ListTableData(TableReference table, int64_t start_index,
                                    int64_t max_results) {
  return impl_->ListTableData(std::move(table), start_index, max_results);
}

QueryResult Emulator::ReadStorageTable(TableReference table, const std::string& restriction) {
  return impl_->ReadStorageTable(std::move(table), restriction);
}

std::vector<InsertError> Emulator::WriteStorageRows(TableReference table,
                                                    const std::vector<FieldSchema>& schema,
                                                    const nlohmann::json& rows, int64_t storage_id,
                                                    bool validate_only) {
  return impl_->WriteStorageRows(std::move(table), schema, rows, storage_id, validate_only);
}

std::vector<InsertError> Emulator::InsertTableData(TableReference table, const nlohmann::json& rows,
                                                   bool skip_invalid_rows,
                                                   bool ignore_unknown_values) {
  return impl_->InsertTableData(std::move(table), rows, skip_invalid_rows, ignore_unknown_values);
}

}  // namespace bigquery_emulator_duckdb
