#pragma once

#include <cstdint>
#include <memory>
#include <optional>
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
// With a partition cutoff, discovery, translation and execution must share the backend's active
// transaction. Current reads use that cutoff; historical reads use their requested timestamp.
// Omitting it does not enable partition expiration outside an execution snapshot.
std::unique_ptr<TableSource> NewDuckDbTableSource(
    Backend& backend, const std::unordered_map<std::string, std::string>& project_ids,
    JobLocation* location, std::unique_ptr<ExternalTableProvider> external = {},
    std::optional<int64_t> partition_cutoff_micros = std::nullopt);

}  // namespace bigquery_emulator_duckdb
