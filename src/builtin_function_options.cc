#include "src/builtin_function_options.h"

#include <cstdint>

#include "googlesql/public/builtin_function.pb.h"
#include "googlesql/public/builtin_function_options.h"
#include "src/language_options.h"

namespace bigquery_emulator_duckdb {

ExcludedFunctionIds BigQueryExcludedFunctionIds() {
  // GoogleSQL implements MONTHS_BETWEEN, but BigQuery has no such built-in function.
  return {googlesql::FN_MONTHS_BETWEEN_DATE, googlesql::FN_MONTHS_BETWEEN_DATETIME};
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
