#include "src/googlesql_collation_duckdb/collation.h"

#include <string>
#include <string_view>

#include "absl/status/statusor.h"
#include "duckdb.h"
#include "googlesql/base/status_macros.h"
#include "src/duckdb_functions/apply.h"
#include "src/duckdb_functions/register.h"
#include "src/duckdb_functions/scalar.h"
#include "src/googlesql_collation/collation.h"

namespace bigquery_emulator_duckdb::collation_duckdb {
using namespace duckdb_functions;
namespace {

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): COLLATE(value, specification).
absl::StatusOr<std::string> Collate(std::string_view value, std::string_view specification,
                                    bool bigquery) {
  GOOGLESQL_RETURN_IF_ERROR(googlesql_collation::CheckSpecification(specification, bigquery));
  return std::string(value);
}

}  // namespace

void RegisterCollationFunctions(duckdb_connection connection, bool bigquery) {
  RegisterWithContext(connection, "bq_collate", {kVarchar, kVarchar}, kVarchar,
                      ApplyWithContext<Collate>, bigquery);
  RegisterWithContext(connection, "bq_collation_key", {kVarchar, kVarchar}, kBlob,
                      ApplyWithContext<googlesql_collation::SortKey>, bigquery);
}

}  // namespace bigquery_emulator_duckdb::collation_duckdb
