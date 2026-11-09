#include "src/language_options.h"

#include "googlesql/public/function.h"
#include "googlesql/public/language_options.h"
#include "googlesql/public/options.pb.h"
#include "src/timestamp_precision.h"

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
             googlesql::FEATURE_MAP_LITERAL,
             googlesql::FEATURE_UUID_TYPE,
             googlesql::FEATURE_TIMESTAMP_NANOS,
             googlesql::FEATURE_TIMESTAMP_PICOS,
             googlesql::FEATURE_CAST_DIFFERENT_ARRAY_TYPES,
             googlesql::FEATURE_ARRAY_EQUALITY,
             googlesql::FEATURE_ARRAY_ORDERING,
         }) {
      options->DisableLanguageFeature(feature);
    }
    // BigQuery cannot group or partition by graph paths, including with DISTINCT, as "Groupable
    // data types" in third_party/bigquery_docs/reference/standard-sql/data-types.md lists.
    options->DisableLanguageFeature(googlesql::FEATURE_GROUP_BY_GRAPH_PATH);
    // BigQuery rejects JSONPath wildcard and descendant operators, including in lax mode.
    options->DisableLanguageFeature(googlesql::FEATURE_JSONPATH_WILDCARDS_AND_DESCENDANT);
    // BigQuery rejects LookupTable with literal, quoted and constant-expression table names.
    options->DisableLanguageFeature(googlesql::FEATURE_LOOKUP_TABLE_TVF);
    // GoogleSQL released JSON_EXISTS functions before BigQuery documented them. BigQuery casts
    // no other type to JSON, leaving that to TO_JSON and PARSE_JSON.
    options->DisableLanguageFeature(googlesql::FEATURE_JSON_EXISTS_FUNCTIONS);
    options->DisableLanguageFeature(googlesql::FEATURE_CAST_TO_JSON_TYPE);
    // BigQuery cannot compare, order, group or partition JSON values; see operators.md and
    // data-types.md under third_party/bigquery_docs/reference/standard-sql/.
    options->DisableLanguageFeature(googlesql::FEATURE_JSON_TYPE_COMPARISON);
    options->DisableLanguageFeature(googlesql::FEATURE_JSON_TYPE_COMPARISON_COERCION);
    // BigQuery rejects PARSE_TIMESTAMP's named precision argument.
    options->DisableLanguageFeature(googlesql::FEATURE_PARSE_TIMESTAMP_WITH_PRECISION_AND_TIMEZONE);
    options->EnableLanguageFeature(googlesql::FEATURE_DISABLE_FLOAT32);
    // BigQuery has EDIT_DISTANCE over BYTES and CREATE TABLE CLONE, which GoogleSQL still marks
    // in development.
    options->EnableLanguageFeature(googlesql::FEATURE_ENABLE_EDIT_DISTANCE_BYTES);
    options->EnableLanguageFeature(googlesql::FEATURE_CREATE_TABLE_CLONE);
    // BigQuery has CREATE EXTERNAL TABLE WITH PARTITION COLUMNS, also still in development.
    options->EnableLanguageFeature(googlesql::FEATURE_CREATE_EXTERNAL_TABLE_WITH_PARTITION_COLUMNS);
    // BigQuery has collations, which GoogleSQL still marks in development, and NULLIF compares
    // under them. The translator rejects the operations whose collation it does not apply.
    options->EnableLanguageFeature(googlesql::FEATURE_COLLATION_SUPPORT);
    options->EnableLanguageFeature(googlesql::FEATURE_USE_OPERATION_COLLATION_FOR_NULLIF);
    options->EnableLanguageFeature(googlesql::FEATURE_COLLATION_IN_WITH_RECURSIVE);
    // BigQuery has SELECT WITH DIFFERENTIAL_PRIVACY, which GoogleSQL still marks in development,
    // without the options and report formats of the features that extend it.
    options->EnableLanguageFeature(googlesql::FEATURE_DIFFERENTIAL_PRIVACY);
    // BigQuery has no ORDER BY ... COLLATE clause; an ordered value carries its collation.
    options->DisableLanguageFeature(googlesql::FEATURE_ORDER_BY_COLLATE);
    // BigQuery rejects collations on arguments of user-defined functions and table functions and
    // in their SQL bodies' results.
    options->DisableLanguageFeature(googlesql::FEATURE_TYPE_ANNOTATIONS_ON_SQL_FUNCTION_ARGUMENTS);
    options->DisableLanguageFeature(
        googlesql::FEATURE_ALL_TYPE_ANNOTATIONS_ON_SQL_FUNCTION_ARGUMENTS);
    // BigQuery supports descriptions on explicitly named view columns.
    options->EnableLanguageFeature(googlesql::FEATURE_CREATE_VIEWS_WITH_COLUMN_OPTIONS);
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

const googlesql::LanguageOptions& AnalyzerLanguageOptions(TimestampPrecision precision) {
  static const googlesql::LanguageOptions* const kNanosecondOptions = [] {
    auto* options = new googlesql::LanguageOptions(GoogleSqlLanguageOptions());
    options->EnableLanguageFeature(googlesql::FEATURE_TIMESTAMP_NANOS);
    return options;
  }();
  return precision == TimestampPrecision::kNanoseconds ? *kNanosecondOptions
                                                       : GoogleSqlLanguageOptions();
}

}  // namespace bigquery_emulator_duckdb
