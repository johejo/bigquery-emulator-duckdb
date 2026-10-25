#pragma once

namespace bigquery_emulator_duckdb::function_catalog {
class Catalog;
}

namespace bigquery_emulator_duckdb::translator {

// Adds the translator's builtin rules and handlers, independently of optional feature adapters.
void AddBuiltinFunctions(function_catalog::Catalog& functions);

}  // namespace bigquery_emulator_duckdb::translator
