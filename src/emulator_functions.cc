#include "src/emulator_functions.h"

#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

#include "absl/status/status.h"
#include "src/function_catalog/catalog.h"
#include "src/function_catalog/resolved_access.h"
#include "src/function_catalog/rules.h"
#include "src/googlesql_collation/collation.h"
#include "src/googlesql_functions_duckdb/math.h"
#include "src/googlesql_functions_duckdb/options.h"
#include "src/googlesql_geography_duckdb/functions.h"
#include "src/translator/functions.h"

namespace bigquery_emulator_duckdb {
namespace {

// A collation is a type annotation, which GoogleSQL propagates to the operations that use
// it; the value is the string itself, once the literal specification is checked.
template <bool UndCiOnly>
std::optional<std::string> Collate(const function_catalog::ScalarCall& call) {
  if (call.arguments.size() != 2) {
    return std::nullopt;
  }
  const auto specification = function_catalog::StringLiteral(call.Argument(1));
  if (!specification) {
    return std::nullopt;
  }
  if constexpr (UndCiOnly) {
    if (!specification->empty() && *specification != "und:ci") {
      throw std::invalid_argument("Collation '" + *specification +
                                  "' in collate function is not supported.");
    }
  } else {
    const absl::Status status = googlesql_collation::CheckSpecification(*specification);
    if (!status.ok()) {
      throw std::invalid_argument(std::string(status.message()));
    }
  }
  return call.arguments.at(0).sql;
}

}  // namespace

// Assembly is expensive; cache each choice of options. Lookups are thread-safe.
const function_catalog::Catalog& EmulatorFunctionCatalog(const functions_duckdb::Options& options,
                                                         bool fetch_object_metadata) {
  static std::mutex mutex;
  static std::map<std::pair<functions_duckdb::Options, bool>,
                  std::unique_ptr<function_catalog::Catalog>>
      catalogs;
  const std::scoped_lock lock(mutex);
  auto& functions = catalogs[{options, fetch_object_metadata}];
  if (functions != nullptr) {
    return *functions;
  }
  functions = std::make_unique<function_catalog::Catalog>(
      options.timestamp_precision,
      function_catalog::QueryRestrictions{
          .json_keys_rejects_safe = options.json_keys_rejects_safe,
          .intersect_except_distinct_only = options.intersect_except_distinct_only,
          .scalar_window_partition_keys_only = options.scalar_window_partition_keys_only,
          .distinct_set_operations_without_structs =
              options.distinct_set_operations_without_structs,
          .distinct_comparisons_without_structs = options.distinct_comparisons_without_structs,
          .arrays_without_null_elements = options.arrays_without_null_elements,
          .restrict_struct_results = options.restrict_struct_results,
      });
  translator::AddBuiltinFunctions(*functions);
  functions->Add("COLLATE",
                 {
                     .translation = function_catalog::Translation::kHandler,
                     .implementations = {function_catalog::Implementation::kSQLInCode},
                     .handler = options.collate_und_ci_only ? Collate<true> : Collate<false>,
                 });
  if (!fetch_object_metadata) {
    functions->Remove("OBJ.FETCH_METADATA");
  }
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
  for (const auto& [name, definition] : geography_duckdb::GeographyAnalytics()) {
    functions->AddAggregates(name, definition, function_catalog::Translation::kAnalytic,
                             function_catalog::Implementation::kS2);
  }
  return *functions;
}

}  // namespace bigquery_emulator_duckdb
