#include "src/googlesql_functions_duckdb/contains_substr.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "duckdb.h"
#include "googlesql/base/status_macros.h"
#include "src/duckdb_functions/register.h"
#include "src/duckdb_functions/scalar.h"
#include "src/googlesql_functions/contains_substr.h"
#include "src/googlesql_functions_duckdb/any_arguments.h"

namespace bigquery_emulator_duckdb::functions_duckdb {
using namespace duckdb_functions;
namespace {

void ContainsSubstr(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  const AnyArguments values(input, {0});
  EachRow(
      info, input, output,
      [&values](const Arguments& arguments) -> absl::StatusOr<std::optional<bool>> {
        if (arguments.IsNull(1)) {
          return absl::OutOfRangeError(
              "The second argument of CONTAINS_SUBSTR() must not be null.");
        }
        GOOGLESQL_ASSIGN_OR_RETURN(const auto value, values.Get(0, arguments.Row()));
        const std::string scope = arguments.IsNull(2) ? "" : arguments.String(2);
        return googlesql_functions::ContainsSubstr(
            value, arguments.String(1),
            arguments.IsNull(2) ? std::nullopt : std::optional<std::string_view>(scope));
      },
      /*nulls=*/false);
}

}  // namespace

void RegisterContainsSubstr(duckdb_connection connection) {
  Register(connection, "bq_contains_substr", std::vector<duckdb_type>{kAny, kVarchar, kVarchar},
           kBoolean, ContainsSubstr,
           /*nulls=*/false);
}

}  // namespace bigquery_emulator_duckdb::functions_duckdb
