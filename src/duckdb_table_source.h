#pragma once

#include <memory>
#include <string>
#include <unordered_map>

#include "src/backend.h"
#include "src/external_table_provider.h"
#include "src/location.h"
#include "src/table_source.h"

namespace bigquery_emulator_duckdb {

// The backend, project aliases and optional job location must outlive the source.
// The caller sweeps expired relations before creating the source when needed. External operations
// are rejected without a provider; native external-table placeholders are never read as data.
std::unique_ptr<TableSource> NewDuckDbTableSource(
    Backend& backend, const std::unordered_map<std::string, std::string>& project_ids,
    JobLocation* location, std::unique_ptr<ExternalTableProvider> external = {});

}  // namespace bigquery_emulator_duckdb
