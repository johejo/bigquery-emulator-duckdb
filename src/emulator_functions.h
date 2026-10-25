#pragma once

namespace bigquery_emulator_duckdb::function_catalog {
class Catalog;
}

namespace bigquery_emulator_duckdb {

// The application's function catalog, assembled once and used read-only by translators.
const function_catalog::Catalog& EmulatorFunctionCatalog();

}  // namespace bigquery_emulator_duckdb
