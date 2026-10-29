#pragma once

#include "src/timestamp_precision.h"

namespace googlesql {
class Function;
class LanguageOptions;
}  // namespace googlesql

namespace bigquery_emulator_duckdb {

// The group of the functions the catalog declares because GoogleSQL does not ship them, which
// BigQuery names in errors such as "No matching signature for function BIGQUERYGOOGLESQL:...".
inline constexpr char kBigQueryGroup[] = "BigQueryGoogleSQL";

// The language settings shared by the catalog's built-in functions and the analyzer, so that a
// statement is not rejected for a feature one of them was not told about. TIMESTAMPs hold
// microseconds, as in BigQuery; only resolution depends on that, which AnalyzerLanguageOptions
// selects.
const googlesql::LanguageOptions& GoogleSqlLanguageOptions();

// GoogleSqlLanguageOptions for analyzing statements whose TIMESTAMPs have `precision`.
const googlesql::LanguageOptions& AnalyzerLanguageOptions(TimestampPrecision precision);

// Whether `function` is a GoogleSQL built-in or a BigQuery function that GoogleSQL does not ship
// and the catalog declares in its place, as opposed to a user-defined one.
bool IsBuiltinFunction(const googlesql::Function& function);

}  // namespace bigquery_emulator_duckdb
