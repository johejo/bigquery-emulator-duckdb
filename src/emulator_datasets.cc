#include <optional>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"
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

using nlohmann::json;

std::vector<std::string> Emulator::ListDatasets(std::string project_id) {
  project_id = ResolveProject(project_id);
  return FirstColumnStrings(Execute(DatasetsQuery(project_id)));
}

std::vector<DatasetListEntry> Emulator::ListDatasetEntries(std::string project_id) {
  project_id = ResolveProject(project_id);
  std::vector<DatasetListEntry> entries;
  for (const json& row : Execute(DatasetEntriesQuery(project_id)).rows) {
    // Keep the wire-field decoding beside the named result fields.
    // cppcheck-suppress useStlAlgorithm
    entries.push_back({
        .dataset_id = row["f"][0]["v"].get<std::string>(),
        .metadata = ParseDatasetMetadata(row["f"][1]["v"]),
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
  return ParseDatasetMetadata(result.rows.at(0)["f"][1]["v"]);
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
