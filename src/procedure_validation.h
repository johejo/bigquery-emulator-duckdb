#pragma once

#include "absl/status/status.h"

namespace googlesql {
class ASTScript;
class TypeFactory;
}  // namespace googlesql

namespace bigquery_emulator_duckdb {

class RoutineCatalog;
struct Routine;

// Resolves a strict procedure's statements and expressions without executing its body.
absl::Status CheckProcedure(const Routine& routine, const googlesql::ASTScript& script,
                            RoutineCatalog& catalog, googlesql::TypeFactory& types);

}  // namespace bigquery_emulator_duckdb
