#include "src/backend_functions/range.h"

#include <optional>
#include <string>

#include "absl/status/statusor.h"
#include "duckdb.h"
#include "googlesql/base/status_macros.h"
#include "googlesql/public/functions/range.h"
#include "src/backend_functions/register.h"
#include "src/backend_functions/scalar.h"

namespace bigquery_emulator_duckdb::backend_functions {
namespace {

// Bound 0 or 1 of the text of a RANGE, which CAST to RANGE then casts to the element type, or NULL
// where it is unbounded.
void RangeBound(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  EachRow(info, input, output,
          [](const Arguments& args) -> absl::StatusOr<std::optional<std::string>> {
            const std::string text = args.String(0);
            GOOGLESQL_ASSIGN_OR_RETURN(const auto bounds, googlesql::ParseRangeBoundaries(text));
            const auto bound = args.Int(1) == 0 ? bounds.start : bounds.end;
            return bound ? std::optional<std::string>(*bound) : std::nullopt;
          });
}

}  // namespace

void RegisterRangeFunctions(duckdb_connection connection) {
  Register(connection, "bq_range_bound", {kVarchar, kBigint}, kVarchar, RangeBound);
}

}  // namespace bigquery_emulator_duckdb::backend_functions
