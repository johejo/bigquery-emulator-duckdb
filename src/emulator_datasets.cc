#include <cstddef>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "src/api_error.h"
#include "src/backend.h"
#include "src/backend_error.h"
#include "src/ddl_write.h"
#include "src/duckdb_catalog.h"
#include "src/emulator.h"
#include "src/references.h"
#include "src/routine.h"
#include "src/table_metadata.h"

namespace bigquery_emulator_duckdb {

std::vector<std::string> Emulator::ListDatasets(std::string project_id) {
  project_id = ResolveProject(project_id);
  return FirstColumnStrings(Execute(DatasetsQuery(project_id)));
}

std::vector<DatasetListEntry> Emulator::ListDatasetEntries(std::string project_id) {
  project_id = ResolveProject(project_id);
  std::vector<DatasetListEntry> entries;
  const QueryResult result = Execute(DatasetEntriesQuery(project_id));
  entries.reserve(result.rows.size());
  for (size_t row = 0; row < result.rows.size(); ++row) {
    entries.push_back({
        .dataset_id = result.rows.String(row, 0),
        .metadata = ParseDatasetMetadata(result.rows.OptionalString(row, 1)),
    });
  }
  return entries;
}

DatasetMetadata Emulator::GetDataset(DatasetReference dataset) {
  dataset.project_id = ResolveProject(dataset.project_id);
  const QueryResult result = Execute(DatasetEntriesQuery(dataset.project_id, dataset.dataset_id));
  if (result.rows.empty()) {
    throw ApiError::NotFound("Not found: Dataset " + dataset.project_id + ":" + dataset.dataset_id);
  }
  return ParseDatasetMetadata(result.rows.OptionalString(0, 1));
}

void Emulator::CreateDataset(DatasetReference dataset, const DatasetMetadata& metadata) {
  dataset.project_id = ResolveProject(dataset.project_id);
  const std::vector<std::string> statements =
      CreateDatasetWrite({.dataset = dataset, .metadata = metadata}).metadata_statements;
  try {
    backend_.ExecuteDdl("CREATE SCHEMA " + QualifiedName(dataset), statements, "");
  } catch (const BackendError& error) {
    if (std::string(error.what()).find("already exists") != std::string::npos) {
      throw ApiError::Duplicate("Already Exists: Dataset " + dataset.project_id + ":" +
                                dataset.dataset_id);
    }
    throw ApiError::Invalid(error.what());
  }
}

void Emulator::UpdateDataset(DatasetReference dataset, const DatasetMetadata& metadata) {
  dataset.project_id = ResolveProject(dataset.project_id);
  GetDataset(dataset);
  const std::vector<std::string> statements = DatasetMetadataStatements(dataset, metadata);
  try {
    backend_.ExecuteDdl(statements.front(), {statements.begin() + 1, statements.end()}, "");
  } catch (const BackendError& error) {
    throw ApiError::Invalid(error.what());
  }
}

void Emulator::DeleteDataset(DatasetReference dataset, bool delete_contents) {
  // CASCADE drops expired relations too, and must not race the lazy expiration sweep.
  std::scoped_lock const lock(expiration_mutex_);
  dataset.project_id = ResolveProject(dataset.project_id);
  GetDataset(dataset);
  if (!delete_contents &&
      (!ListTables(dataset).empty() || !Execute(RoutinesQuery(dataset)).rows.empty())) {
    throw ApiError::Invalid("Dataset " + dataset.project_id + ":" + dataset.dataset_id +
                            " is still in use");
  }
  try {
    backend_.ExecuteDdl(
        "DROP SCHEMA " + QualifiedName(dataset) + (delete_contents ? " CASCADE" : ""),
        DropDatasetWrite(dataset).metadata_statements, "");
  } catch (const BackendError& error) {
    throw ApiError::Invalid(error.what());
  }
}

}  // namespace bigquery_emulator_duckdb
