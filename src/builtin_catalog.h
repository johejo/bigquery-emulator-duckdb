#pragma once

#include "src/builtin_function_options.h"

namespace googlesql {
class SimpleCatalog;
}  // namespace googlesql

namespace bigquery_emulator_duckdb {

// Shared, immutable after construction, and cached for the process lifetime per
// exclusion list. The caller borrows the catalog; it must not modify or delete it.
googlesql::SimpleCatalog* BuiltinCatalog(const ExcludedFunctionIds& excluded_function_ids);

}  // namespace bigquery_emulator_duckdb
