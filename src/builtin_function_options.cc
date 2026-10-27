#include "src/builtin_function_options.h"

#include "googlesql/public/builtin_function.pb.h"

namespace bigquery_emulator_duckdb {

ExcludedFunctionIds BigQueryExcludedFunctionIds() {
  // GoogleSQL implements MONTHS_BETWEEN, but BigQuery has no such built-in function.
  return {googlesql::FN_MONTHS_BETWEEN_DATE, googlesql::FN_MONTHS_BETWEEN_DATETIME};
}

}  // namespace bigquery_emulator_duckdb
