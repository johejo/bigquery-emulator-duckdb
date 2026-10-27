#pragma once

#include <cstdint>
#include <vector>

namespace bigquery_emulator_duckdb {

// GoogleSQL signature IDs to exclude from analysis. Empty exposes all built-ins.
using ExcludedFunctionIds = std::vector<int64_t>;
ExcludedFunctionIds BigQueryExcludedFunctionIds();

}  // namespace bigquery_emulator_duckdb
