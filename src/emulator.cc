#include "src/emulator.h"

#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "src/backend.h"
#include "src/backend_initializer.h"
#include "src/builtin_function_options.h"
#include "src/external_query.h"
#include "src/googlesql_functions_duckdb/options.h"
#include "src/googlesql_geography/options.h"
#include "src/object_ref.h"
#include "src/project.h"
#include "src/service_backend.h"
#include "src/service_table_provider.h"
#include "src/sql_runtime.h"

namespace bigquery_emulator_duckdb {

Emulator::Emulator(std::string data_dir, const std::vector<Project>& projects,
                   const std::optional<std::string>& session_user,
                   const BackendInitializer& initialize,
                   const std::vector<ExternalConnection>& connections,
                   const googlesql_geography::Options& geography,
                   const functions_duckdb::Options& functions,
                   ExcludedFunctionIds excluded_function_ids, FunctionDeclarations declarations)
    : SqlRuntime(
          std::move(data_dir), projects, session_user, ServiceBackendInitializer(initialize),
          {
              .fetch =
                  [this](const std::string& objectref, const std::string& project) {
                    return FetchObjectMetadata(gcs_client_, connections_.conninfo, objectref,
                                               project);
                  },
              .has_connection =
                  [this](const std::string& key) { return connections_.conninfo.contains(key); },
          },
          InitializeServiceDatabase,
          [this](Backend&) { return NewServiceTableProvider(connections_, gcs_client_); },
          geography, functions, std::move(excluded_function_ids), declarations) {
  // These callbacks are used by statements, never during runtime construction.
  for (const ExternalConnection& connection : connections) {
    // cppcheck-suppress useStlAlgorithm
    if (!connections_.conninfo.emplace(connection.key, connection.conninfo).second) {
      throw std::invalid_argument("Duplicate connection: " + connection.key);
    }
  }
}

}  // namespace bigquery_emulator_duckdb
