#pragma once

#include <memory>
#include <string>
#include <vector>

#include "absl/status/statusor.h"
#include "src/catalog.h"

namespace googlesql {
class TypeFactory;
}  // namespace googlesql

namespace bigquery_emulator_duckdb {

// Whether `parts`, a table path split by SplitTablePath, names a view in INFORMATION_SCHEMA:
// [PROJECT.][DATASET or `region-REGION`.]INFORMATION_SCHEMA.VIEW.
bool IsInformationSchemaPath(const std::vector<std::string>& parts);

// Builds the INFORMATION_SCHEMA view `parts` names, with the rows it has now. SCHEMATA, TABLES,
// COLUMNS and COLUMN_FIELD_PATHS are supported. The emulator keeps no dataset locations and
// reports every dataset in the US, so `region-us` covers every dataset and other regions none.
absl::StatusOr<std::unique_ptr<SqlTable>> InformationSchemaView(
    const std::vector<std::string>& parts, TableSource& source,
    googlesql::TypeFactory* type_factory, const std::string& default_project,
    const std::string& default_dataset);

}  // namespace bigquery_emulator_duckdb
