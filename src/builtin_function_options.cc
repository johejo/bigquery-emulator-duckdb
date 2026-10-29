#include "src/builtin_function_options.h"

#include <cstdint>

#include "googlesql/public/builtin_function.pb.h"
#include "googlesql/public/builtin_function_options.h"
#include "src/language_options.h"

namespace bigquery_emulator_duckdb {

ExcludedFunctionIds BigQueryExcludedFunctionIds() {
  return {
      // BigQuery coerces every CBRT input to FLOAT64; GoogleSQL also has decimal overloads.
      googlesql::FN_CBRT_NUMERIC,
      googlesql::FN_CBRT_BIGNUMERIC,
      // GoogleSQL implements these functions, but BigQuery has no such built-ins.
      googlesql::FN_MONTHS_BETWEEN_DATE,
      googlesql::FN_MONTHS_BETWEEN_DATETIME,
      googlesql::FN_ARRAY_AVG_INT32,
      googlesql::FN_ARRAY_AVG_INT64,
      googlesql::FN_ARRAY_AVG_UINT32,
      googlesql::FN_ARRAY_AVG_UINT64,
      googlesql::FN_ARRAY_AVG_FLOAT,
      googlesql::FN_ARRAY_AVG_DOUBLE,
      googlesql::FN_ARRAY_AVG_NUMERIC,
      googlesql::FN_ARRAY_AVG_BIGNUMERIC,
      googlesql::FN_ARRAY_AVG_INTERVAL,
  };
}

googlesql::BuiltinFunctionOptions AnalyzerBuiltinFunctionOptions(
    const ExcludedFunctionIds& excluded_function_ids) {
  googlesql::BuiltinFunctionOptions options(GoogleSqlLanguageOptions());
  // Use the backend implementation, not SQL expansion (which needs nanosecond arithmetic).
  options.rewrite_enabled = {
      {googlesql::FN_MONTHS_BETWEEN_DATE, false},
      {googlesql::FN_MONTHS_BETWEEN_DATETIME, false},
  };
  for (const int64_t id : excluded_function_ids) {
    options.exclude_function_ids.insert(static_cast<googlesql::FunctionSignatureId>(id));
  }
  return options;
}

}  // namespace bigquery_emulator_duckdb
