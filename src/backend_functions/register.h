#pragma once

#include <optional>
#include <vector>

#include "duckdb.h"

namespace bigquery_emulator_duckdb::backend_functions {

// Registers `function` under `name`, taking `parameters` and returning `result`. Unless `nulls`
// is false, DuckDB makes the result NULL for a NULL argument without calling `function`. A
// `volatile_result` keeps DuckDB from folding or sharing calls with the same arguments.
// Throws BackendError when DuckDB rejects it.
void Register(duckdb_connection connection, const char* name,
              const std::vector<duckdb_logical_type>& parameters, duckdb_logical_type result,
              duckdb_scalar_function_t function, bool nulls = true,
              std::optional<duckdb_type> varargs = std::nullopt, bool volatile_result = false);
void Register(duckdb_connection connection, const char* name,
              const std::vector<duckdb_type>& parameters, duckdb_logical_type result,
              duckdb_scalar_function_t function, bool nulls = true,
              std::optional<duckdb_type> varargs = std::nullopt, bool volatile_result = false);
void Register(duckdb_connection connection, const char* name,
              const std::vector<duckdb_type>& parameters, duckdb_type result,
              duckdb_scalar_function_t function, bool nulls = true,
              std::optional<duckdb_type> varargs = std::nullopt, bool volatile_result = false);

}  // namespace bigquery_emulator_duckdb::backend_functions
