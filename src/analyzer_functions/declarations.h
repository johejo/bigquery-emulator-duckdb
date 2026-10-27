#pragma once

namespace googlesql {
class SimpleCatalog;
}  // namespace googlesql

namespace bigquery_emulator_duckdb::analyzer_functions {

// Feature-specific declaration builders, used only by the assembly in functions.cc.
void AddTextAnalysisFunctions(googlesql::SimpleCatalog& catalog);
void AddObjectRefFunctions(googlesql::SimpleCatalog& catalog);
void AddRangeSessionize(googlesql::SimpleCatalog& catalog);

}  // namespace bigquery_emulator_duckdb::analyzer_functions
