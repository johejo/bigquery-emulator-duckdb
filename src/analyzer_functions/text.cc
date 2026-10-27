#include <cstdint>
#include <stdexcept>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "googlesql/public/function.h"
#include "googlesql/public/function_signature.h"
#include "googlesql/public/simple_catalog.h"
#include "googlesql/public/type.h"
#include "googlesql/public/value.h"
#include "src/analyzer_functions/declarations.h"
#include "src/language_options.h"

namespace bigquery_emulator_duckdb::analyzer_functions {

void AddTextAnalysisFunctions(googlesql::SimpleCatalog& catalog) {
  // TEXT_ANALYZE's analyzer and analyzer_options are named only. Their defaults keep an omitted
  // one apart from an explicit NULL, which makes the result NULL; options of {} are no options.
  googlesql::FunctionArgumentTypeOptions analyzer(googlesql::FunctionArgumentType::OPTIONAL);
  analyzer.set_argument_name("analyzer", googlesql::kNamedOnly);
  analyzer.set_default(googlesql::Value::String("LOG_ANALYZER"));
  googlesql::FunctionArgumentTypeOptions analyzer_options(
      googlesql::FunctionArgumentType::OPTIONAL);
  analyzer_options.set_argument_name("analyzer_options", googlesql::kNamedOnly);
  analyzer_options.set_default(googlesql::Value::String("{}"));
  catalog.AddOwnedFunction(new googlesql::Function(
      "text_analyze", kBigQueryGroup, googlesql::Function::SCALAR,
      {
          googlesql::FunctionSignature(
              googlesql::FunctionArgumentType(googlesql::types::StringArrayType()),
              {
                  googlesql::FunctionArgumentType(googlesql::types::StringType()),
                  googlesql::FunctionArgumentType(googlesql::types::StringType(), analyzer),
                  googlesql::FunctionArgumentType(googlesql::types::StringType(), analyzer_options),
              },
              /*context_id=*/static_cast<int64_t>(0)),
      }));
  const googlesql::Type* term = nullptr;
  if (!catalog.type_factory()
           ->MakeStructType(
               {
                   {"term", googlesql::types::StringType()},
                   {"count", googlesql::types::Int64Type()},
               },
               &term)
           .ok()) {
    throw std::runtime_error("failed to make the type of BAG_OF_WORDS");
  }
  const absl::StatusOr<const googlesql::ArrayType*> terms =
      catalog.type_factory()->MakeArrayType(term);
  if (!terms.ok()) {
    throw std::runtime_error(terms.status().ToString());
  }
  catalog.AddOwnedFunction(new googlesql::Function(
      "bag_of_words", kBigQueryGroup, googlesql::Function::SCALAR,
      {
          googlesql::FunctionSignature(
              googlesql::FunctionArgumentType(*terms),
              {googlesql::FunctionArgumentType(googlesql::types::StringArrayType())},
              /*context_id=*/static_cast<int64_t>(0)),
      }));
  // TF_IDF takes only OVER (); the translator rejects PARTITION BY and SAFE as BigQuery does. The
  // analyzer checks literal limits, and the backend query parameters.
  const googlesql::Type* score = nullptr;
  if (!catalog.type_factory()
           ->MakeStructType(
               {
                   {"term", googlesql::types::StringType()},
                   {"tf_idf", googlesql::types::DoubleType()},
               },
               &score)
           .ok()) {
    throw std::runtime_error("failed to make the type of TF_IDF");
  }
  const absl::StatusOr<const googlesql::ArrayType*> scores =
      catalog.type_factory()->MakeArrayType(score);
  if (!scores.ok()) {
    throw std::runtime_error(scores.status().ToString());
  }
  googlesql::FunctionArgumentTypeOptions max_distinct_tokens(
      googlesql::FunctionArgumentType::OPTIONAL);
  googlesql::FunctionArgumentTypeOptions frequency_threshold(
      googlesql::FunctionArgumentType::OPTIONAL);
  // Only the legacy constness reports "must be a literal or query parameter", as BigQuery does.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
  max_distinct_tokens.set_must_be_constant();
  frequency_threshold.set_must_be_constant();
#pragma clang diagnostic pop
  max_distinct_tokens.set_must_be_non_null().set_min_value(1).set_max_value(1048576);
  frequency_threshold.set_must_be_non_null().set_min_value(0);
  catalog.AddOwnedFunction(new googlesql::Function(
      "tf_idf", kBigQueryGroup, googlesql::Function::ANALYTIC,
      {
          googlesql::FunctionSignature(
              googlesql::FunctionArgumentType(*scores),
              {
                  googlesql::FunctionArgumentType(googlesql::types::StringArrayType()),
                  googlesql::FunctionArgumentType(googlesql::types::Int64Type(),
                                                  max_distinct_tokens),
                  googlesql::FunctionArgumentType(googlesql::types::Int64Type(),
                                                  frequency_threshold),
              },
              /*context_id=*/static_cast<int64_t>(0)),
      },
      googlesql::FunctionOptions(googlesql::FunctionOptions::ORDER_UNSUPPORTED,
                                 /*window_framing_support_in=*/false)));
}

}  // namespace bigquery_emulator_duckdb::analyzer_functions
