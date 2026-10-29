#pragma once

#include "src/googlesql_functions_duckdb/options.h"

namespace bigquery_emulator_duckdb::function_catalog {
class Catalog;
}

namespace bigquery_emulator_duckdb {

// The application's function catalog for the backend functions' `options`, which select
// BigQuery's or GoogleSQL's semantics. Assembled once for each and used read-only by translators.
const function_catalog::Catalog& EmulatorFunctionCatalog(const functions_duckdb::Options& options);

}  // namespace bigquery_emulator_duckdb
