#pragma once

#include <memory>
#include <string>
#include <unordered_map>

namespace bigquery_emulator_duckdb {

class Backend;
class JobLocation;
class TableSource;
struct ExternalConnections;

// The backend, connections, project aliases and optional job location must outlive the source.
// The caller sweeps expired relations before creating the source when needed.
std::unique_ptr<TableSource> NewDuckDbTableSource(
    Backend& backend, ExternalConnections& connections,
    const std::unordered_map<std::string, std::string>& project_ids, JobLocation* location);

}  // namespace bigquery_emulator_duckdb
