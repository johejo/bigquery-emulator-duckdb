#include "src/analyzer_functions/functions.h"

#include <cstdint>

#include "googlesql/public/function.h"
#include "googlesql/public/function_signature.h"
#include "googlesql/public/simple_catalog.h"
#include "googlesql/public/type.h"
#include "googlesql/public/value.h"
#include "src/analyzer_functions/declarations.h"
#include "src/language_options.h"

namespace bigquery_emulator_duckdb::analyzer_functions {

void AddBigQueryFunctionDeclarations(googlesql::SimpleCatalog& catalog) {
  // BigQuery functions that GoogleSQL does not ship, declared with the signatures the
  // translator supports.
  googlesql::FunctionArgumentTypeOptions json_scope(googlesql::FunctionArgumentType::OPTIONAL);
  json_scope.set_argument_name("json_scope", googlesql::kNamedOnly);
  json_scope.set_default(googlesql::Value::String("JSON_VALUES"));
  catalog.AddOwnedFunction(new googlesql::Function(
      "contains_substr", kBigQueryGroup, googlesql::Function::SCALAR,
      {
          googlesql::FunctionSignature(
              googlesql::FunctionArgumentType(googlesql::types::BoolType()),
              {
                  googlesql::FunctionArgumentType(googlesql::ARG_KIND_EXPR_ARBITRARY),
                  googlesql::FunctionArgumentType(googlesql::types::StringType()),
                  googlesql::FunctionArgumentType(googlesql::types::StringType(), json_scope),
              },
              /*context_id=*/static_cast<int64_t>(0)),
      }));
  for (const char* name : {"max_by", "min_by"}) {
    catalog.AddOwnedFunction(new googlesql::Function(
        name, kBigQueryGroup, googlesql::Function::AGGREGATE,
        {
            googlesql::FunctionSignature(
                googlesql::FunctionArgumentType(googlesql::ARG_KIND_EXPR_ANY_1),
                {
                    googlesql::FunctionArgumentType(googlesql::ARG_KIND_EXPR_ANY_1),
                    googlesql::FunctionArgumentType(googlesql::ARG_KIND_EXPR_ANY_2),
                },
                /*context_id=*/static_cast<int64_t>(0)),
        },
        googlesql::FunctionOptions(googlesql::FunctionOptions::ORDER_OPTIONAL,
                                   /*window_framing_support_in=*/true)));
  }
  AddTextAnalysisFunctions(catalog);
  AddObjectRefFunctions(catalog);
  AddRangeSessionize(catalog);
}

}  // namespace bigquery_emulator_duckdb::analyzer_functions
