#include "src/emulator_functions.h"

#include <map>
#include <memory>
#include <mutex>

#include "src/function_catalog/catalog.h"
#include "src/function_catalog/rules.h"
#include "src/googlesql_functions_duckdb/math.h"
#include "src/googlesql_functions_duckdb/options.h"
#include "src/googlesql_geography_duckdb/functions.h"
#include "src/translator/functions.h"

namespace bigquery_emulator_duckdb {

// Assembly is expensive; cache each choice of options. Lookups are thread-safe.
const function_catalog::Catalog& EmulatorFunctionCatalog(const functions_duckdb::Options& options) {
  static std::mutex mutex;
  static std::map<functions_duckdb::Options, std::unique_ptr<function_catalog::Catalog>> catalogs;
  const std::scoped_lock lock(mutex);
  auto& functions = catalogs[options];
  if (functions != nullptr) {
    return *functions;
  }
  functions = std::make_unique<function_catalog::Catalog>();
  translator::AddBuiltinFunctions(*functions);
  for (const auto& [name, definition] : functions_duckdb::TrigonometricFunctions()) {
    functions->AddRules(name, definition, function_catalog::Implementation::kGoogleSQL);
  }
  for (const auto& [name, definition] : geography_duckdb::GeographyFunctions()) {
    functions->AddRules(name, definition, function_catalog::Implementation::kS2);
  }
  for (const auto& [name, definition] : geography_duckdb::GeographyAggregates()) {
    functions->AddAggregates(name, definition, function_catalog::Translation::kAggregate,
                             function_catalog::Implementation::kS2);
  }
  return *functions;
}

}  // namespace bigquery_emulator_duckdb
