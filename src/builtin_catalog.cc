#include "src/builtin_catalog.h"

#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>

#include "absl/status/status.h"
#include "googlesql/public/builtin_function_options.h"
#include "googlesql/public/simple_catalog.h"
#include "googlesql/public/type.h"
#include "src/analyzer_functions/functions.h"
#include "src/builtin_function_options.h"

namespace bigquery_emulator_duckdb {

// Built once per exclusion list: registration is expensive, and lookups are thread-safe.
googlesql::SimpleCatalog* BuiltinCatalog(const ExcludedFunctionIds& excluded_function_ids) {
  static std::mutex mutex;
  static std::map<ExcludedFunctionIds, std::unique_ptr<googlesql::SimpleCatalog>> catalogs;
  const std::scoped_lock lock(mutex);
  auto& cached = catalogs[excluded_function_ids];
  if (cached != nullptr) {
    return cached.get();
  }
  cached.reset([&] {
    auto* catalog = new googlesql::SimpleCatalog("builtin", new googlesql::TypeFactory());
    const googlesql::BuiltinFunctionOptions options =
        AnalyzerBuiltinFunctionOptions(excluded_function_ids);
    const absl::Status status = catalog->AddBuiltinFunctionsAndTypes(options);
    if (!status.ok()) {
      throw std::runtime_error(status.ToString());
    }
    analyzer_functions::AddBigQueryFunctionDeclarations(*catalog);
    return catalog;
  }());
  return cached.get();
}

}  // namespace bigquery_emulator_duckdb
