#pragma once

#include <memory>
#include <string>
#include <vector>

#include "absl/status/statusor.h"
#include "src/catalog_table.h"

namespace googlesql {
class TypeFactory;
}  // namespace googlesql

namespace bigquery_emulator_duckdb {

class TableSource;

// Whether `parts`, a table path split by SplitTablePath, names a view in INFORMATION_SCHEMA:
// [PROJECT.][DATASET or `region-REGION`.]INFORMATION_SCHEMA.VIEW.
bool IsInformationSchemaPath(const std::vector<std::string>& parts);

// Builds the INFORMATION_SCHEMA view `parts` names, with the rows it has now. SCHEMATA,
// SCHEMATA_OPTIONS, TABLES, TABLE_SNAPSHOTS, TABLE_OPTIONS, PARTITIONS, VIEWS, COLUMNS,
// COLUMN_FIELD_PATHS, ROUTINES and PARAMETERS are supported. Dataset and region qualifiers select
// the datasets to describe and require the query to run in their location.
absl::StatusOr<std::unique_ptr<SqlTable>> InformationSchemaView(
    const std::vector<std::string>& parts, TableSource& source,
    googlesql::TypeFactory* type_factory, const std::string& default_project,
    const std::string& default_dataset);

}  // namespace bigquery_emulator_duckdb
