#include "src/language_options.h"

#include "googlesql/public/function.h"
#include "googlesql/public/language_options.h"
#include "googlesql/public/options.pb.h"

namespace bigquery_emulator_duckdb {

bool IsBuiltinFunction(const googlesql::Function& function) {
  return function.IsGoogleSQLBuiltin() || function.GetGroup() == kBigQueryGroup;
}

const googlesql::LanguageOptions& GoogleSqlLanguageOptions() {
  static const googlesql::LanguageOptions* const kLanguageOptions = [] {
    auto* options = new googlesql::LanguageOptions();
    // Some syntax BigQuery accepts is gated behind a language feature that the default options
    // leave off, QUALIFY among it, so every released feature is turned on and known differences
    // from BigQuery are turned off below.
    options->EnableMaximumLanguageFeatures();
    options->EnableReservableKeyword("GRAPH_TABLE").IgnoreError();
    // Some of those features add types and precision that BigQuery lacks rather than syntax, so
    // they are turned off again: BigQuery has no MAP, UUID or FLOAT32, and its timestamps hold
    // microseconds, not nanoseconds or picoseconds. BigQuery also rejects explicit casts between
    // arrays with different element types, including SAFE_CAST and arrays inside structs, and
    // comparing or ordering arrays, including arrays inside structs.
    for (const googlesql::LanguageFeature feature : {
             googlesql::FEATURE_MAP_TYPE,
             googlesql::FEATURE_UUID_TYPE,
             googlesql::FEATURE_TIMESTAMP_NANOS,
             googlesql::FEATURE_TIMESTAMP_PICOS,
             googlesql::FEATURE_CAST_DIFFERENT_ARRAY_TYPES,
             googlesql::FEATURE_ARRAY_EQUALITY,
             googlesql::FEATURE_ARRAY_ORDERING,
         }) {
      options->DisableLanguageFeature(feature);
    }
    // GoogleSQL released JSON_EXISTS functions before BigQuery documented them. BigQuery casts
    // no other type to JSON, leaving that to TO_JSON and PARSE_JSON.
    options->DisableLanguageFeature(googlesql::FEATURE_JSON_EXISTS_FUNCTIONS);
    options->DisableLanguageFeature(googlesql::FEATURE_CAST_TO_JSON_TYPE);
    options->EnableLanguageFeature(googlesql::FEATURE_DISABLE_FLOAT32);
    // BigQuery has EDIT_DISTANCE over BYTES and CREATE TABLE CLONE, which GoogleSQL still marks
    // in development.
    options->EnableLanguageFeature(googlesql::FEATURE_ENABLE_EDIT_DISTANCE_BYTES);
    options->EnableLanguageFeature(googlesql::FEATURE_CREATE_TABLE_CLONE);
    // BigQuery supports incremental tag options; handlers reject unsupported operators.
    options->EnableLanguageFeature(googlesql::FEATURE_ENABLE_ALTER_ARRAY_OPTIONS);
    // BigQuery is the external product: INT64 and FLOAT64 rather than the internal type set.
    options->set_product_mode(googlesql::PRODUCT_EXTERNAL);
    // The analyzer accepts only queries by default, but the emulator also runs DDL and DML.
    options->SetSupportsAllStatementKinds();
    return options;
  }();
  return *kLanguageOptions;
}

}  // namespace bigquery_emulator_duckdb
