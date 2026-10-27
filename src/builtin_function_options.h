#pragma once

#include <cstdint>
#include <vector>

namespace googlesql {
class BuiltinFunctionOptions;
}  // namespace googlesql

namespace bigquery_emulator_duckdb {

// Additional analyzer declarations, independent of exclusions and backend behavior.
enum class FunctionDeclarations : std::uint8_t { kGoogleSQL, kBigQuery };

// GoogleSQL signature IDs to exclude from analysis. Empty exposes all built-ins.
using ExcludedFunctionIds = std::vector<int64_t>;
ExcludedFunctionIds BigQueryExcludedFunctionIds();

// Analyzer registration policy, including backend evaluation instead of SQL rewrites.
googlesql::BuiltinFunctionOptions AnalyzerBuiltinFunctionOptions(
    const ExcludedFunctionIds& excluded_function_ids);

}  // namespace bigquery_emulator_duckdb
