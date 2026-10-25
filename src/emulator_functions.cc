#include "src/emulator_functions.h"

#include "src/function_catalog/catalog.h"
#include "src/function_catalog/rules.h"
#include "src/googlesql_geography_duckdb/functions.h"
#include "src/translator/functions.h"

namespace bigquery_emulator_duckdb {

const function_catalog::Catalog& EmulatorFunctionCatalog() {
  static const auto* const kFunctions = [] {
    auto* functions = new function_catalog::Catalog();
    translator::AddBuiltinFunctions(*functions);
    for (const auto& [name, definition] : geography_duckdb::GeographyFunctions()) {
      functions->AddRules(name, definition, function_catalog::Implementation::kS2);
    }
    return functions;
  }();
  return *kFunctions;
}

}  // namespace bigquery_emulator_duckdb
