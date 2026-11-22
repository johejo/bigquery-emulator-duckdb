#include "src/googlesql_functions_duckdb/vector_search.h"

#include <optional>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "duckdb.h"
#include "src/duckdb_functions/register.h"
#include "src/duckdb_functions/scalar.h"
#include "src/googlesql_functions/vector_search_options.h"

namespace bigquery_emulator_duckdb::functions_duckdb {
using namespace duckdb_functions;
namespace {

absl::StatusOr<std::optional<bool>> CheckOptions(const Arguments& arguments) {
  if (auto status = googlesql_functions::CheckVectorSearchOptions(arguments.String(0));
      !status.ok()) {
    return status;
  }
  return true;
}

}  // namespace

void RegisterVectorSearch(duckdb_connection connection) {
  Register(connection, "bq_vector_search_options", std::vector<duckdb_type>{kVarchar}, kBoolean,
           EachRow<CheckOptions>);
}

}  // namespace bigquery_emulator_duckdb::functions_duckdb
