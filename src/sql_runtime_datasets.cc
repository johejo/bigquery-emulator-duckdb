#include <cstddef>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "src/api_error.h"
#include "src/backend.h"
#include "src/backend_error.h"
#include "src/ddl_write.h"
#include "src/duckdb_catalog.h"
#include "src/references.h"
#include "src/routine.h"
#include "src/sql_runtime.h"
#include "src/table_metadata.h"

namespace bigquery_emulator_duckdb {

std::vector<std::string> SqlRuntime::ListDatasets(std::string project_id) {
  project_id = ResolveProject(project_id);
  return FirstColumnStrings(Execute(DatasetsQuery(project_id)));
}

std::vector<DatasetListEntry> SqlRuntime::ListDatasetEntries(std::string project_id) {
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

DatasetReference SqlRuntime::ResolveDatasetReference(DatasetReference dataset) {
  dataset.project_id = ResolveProject(dataset.project_id);
  if (const auto found = FindDatasetReference(backend_, dataset)) {
    return *found;
  }
  throw ApiError::NotFound("Not found: Dataset " + dataset.project_id + ":" + dataset.dataset_id);
}

TableReference SqlRuntime::ResolveTableReference(TableReference table, bool create) {
  table.project_id = ResolveProject(table.project_id);
  if (const auto found = FindTableReference(backend_, table)) {
    return *found;
  }
  table.dataset_id =
      ResolveDatasetReference({.project_id = table.project_id, .dataset_id = table.dataset_id})
          .dataset_id;
  if (!create) {
    throw ApiError::NotFound("Not found: Table " + TableName(table));
  }
  CheckCaseInsensitiveTableName(backend_, table);
  return table;
}

DatasetMetadata SqlRuntime::GetDataset(DatasetReference dataset) {
  dataset = ResolveDatasetReference(std::move(dataset));
  const QueryResult result = Execute(DatasetEntriesQuery(dataset.project_id, dataset.dataset_id));
  if (result.rows.empty()) {
    throw ApiError::NotFound("Not found: Dataset " + dataset.project_id + ":" + dataset.dataset_id);
  }
  return ParseDatasetMetadata(result.rows.OptionalString(0, 1));
}

void SqlRuntime::CreateDataset(DatasetReference dataset, const DatasetMetadata& metadata) {
  std::scoped_lock const lock(expiration_mutex_);
  dataset.project_id = ResolveProject(dataset.project_id);
  if (FindDatasetReference(backend_, dataset)) {
    throw ApiError::Duplicate("Already Exists: Dataset " + dataset.project_id + ":" +
                              dataset.dataset_id);
  }
  CheckCaseInsensitiveDataset(backend_, dataset, metadata);
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

void SqlRuntime::UpdateDataset(DatasetReference dataset, const DatasetMetadata& metadata) {
  std::scoped_lock const lock(expiration_mutex_);
  dataset = ResolveDatasetReference(std::move(dataset));
  GetDataset(dataset);
  CheckCaseInsensitiveDataset(backend_, dataset, metadata);
  const std::vector<std::string> statements = DatasetMetadataStatements(dataset, metadata);
  try {
    backend_.ExecuteDdl(statements.front(), {statements.begin() + 1, statements.end()}, "");
  } catch (const BackendError& error) {
    throw ApiError::Invalid(error.what());
  }
}

void SqlRuntime::DeleteDataset(DatasetReference dataset, bool delete_contents) {
  // CASCADE drops expired relations too, and must not race the lazy expiration sweep.
  std::scoped_lock const lock(expiration_mutex_);
  dataset = ResolveDatasetReference(std::move(dataset));
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
