#pragma once

#include <memory>
#include <string>

namespace googlesql {
class SimpleCatalog;
class TableValuedFunction;
class TypeFactory;
}  // namespace googlesql

namespace bigquery_emulator_duckdb {
class TableSource;

namespace analyzer_functions {

// Supplements GoogleSQL's built-ins with BigQuery declarations for analysis, not
// translation rules or executable implementations. Types belong to the catalog's factory.
void AddBigQueryFunctionDeclarations(googlesql::SimpleCatalog& catalog);

// The caller owns the TVF. The source and type factory must outlive it; its result
// schema is described by the external database during statement analysis.
std::unique_ptr<googlesql::TableValuedFunction> MakeExternalQueryFunction(
    TableSource& source, std::string default_project, googlesql::TypeFactory* type_factory);

}  // namespace analyzer_functions
}  // namespace bigquery_emulator_duckdb
