#include "src/emulator_functions.h"

#include "src/function_catalog/catalog.h"
#include "src/function_catalog/rules.h"
#include "src/googlesql_functions_duckdb/math.h"
#include "src/googlesql_geography_duckdb/functions.h"
#include "src/translator/functions.h"

namespace bigquery_emulator_duckdb {

const function_catalog::Catalog& EmulatorFunctionCatalog() {
  static const auto* const kFunctions = [] {
    auto* functions = new function_catalog::Catalog();
    translator::AddBuiltinFunctions(*functions);
    for (const auto& [name, definition] : functions_duckdb::TrigonometricFunctions()) {
      functions->AddRules(name, definition, function_catalog::Implementation::kGoogleSQL);
    }
    for (const auto& [name, definition] : geography_duckdb::GeographyFunctions()) {
      functions->AddRules(name, definition, function_catalog::Implementation::kS2);
    }
    for (const auto& [name, definition] : geography_duckdb::GeographyAggregates()) {
      functions->Add(name, {
                               .translation = function_catalog::Translation::kAggregate,
                               .uses = definition.uses,
                               .implementations =
                                   {
                                       function_catalog::Implementation::kAggregate,
                                       function_catalog::Implementation::kS2,
                                   },
                               .aggregates = definition.rules,
                           });
    }
    return functions;
  }();
  return *kFunctions;
}

}  // namespace bigquery_emulator_duckdb
