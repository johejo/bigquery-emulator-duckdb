#include "src/backend_functions/register.h"

#include <optional>
#include <string>
#include <vector>

#include "src/backend_error.h"
#include "src/duckdb_handle.h"

namespace bigquery_emulator_duckdb::backend_functions {

void Register(duckdb_connection connection, const char* name,
              const std::vector<duckdb_logical_type>& parameters, duckdb_logical_type result,
              duckdb_scalar_function_t function, bool nulls, std::optional<duckdb_type> varargs,
              bool volatile_result) {
  Handle<duckdb_scalar_function, duckdb_destroy_scalar_function> scalar(
      duckdb_create_scalar_function());
  duckdb_scalar_function_set_name(scalar.get(), name);
  for (duckdb_logical_type parameter : parameters) {
    duckdb_scalar_function_add_parameter(scalar.get(), parameter);
  }
  if (varargs) {
    LogicalType type(duckdb_create_logical_type(*varargs));
    duckdb_scalar_function_set_varargs(scalar.get(), type.get());
  }
  duckdb_scalar_function_set_return_type(scalar.get(), result);
  duckdb_scalar_function_set_function(scalar.get(), function);
  if (!nulls) {
    duckdb_scalar_function_set_special_handling(scalar.get());
  }
  if (volatile_result) {
    duckdb_scalar_function_set_volatile(scalar.get());
  }
  if (duckdb_register_scalar_function(connection, scalar.get()) == DuckDBError) {
    throw BackendError(std::string("DuckDB failed to register ") + name);
  }
}

void Register(duckdb_connection connection, const char* name,
              const std::vector<duckdb_type>& parameters, duckdb_logical_type result,
              duckdb_scalar_function_t function, bool nulls, std::optional<duckdb_type> varargs,
              bool volatile_result) {
  std::vector<LogicalType> owned;
  std::vector<duckdb_logical_type> types;
  owned.reserve(parameters.size());
  types.reserve(parameters.size());
  for (const duckdb_type parameter : parameters) {
    // Keep ownership and the corresponding borrowed handle together.
    // cppcheck-suppress useStlAlgorithm
    types.push_back(owned.emplace_back(duckdb_create_logical_type(parameter)).get());
  }
  Register(connection, name, types, result, function, nulls, varargs, volatile_result);
}

void Register(duckdb_connection connection, const char* name,
              const std::vector<duckdb_type>& parameters, duckdb_type result,
              duckdb_scalar_function_t function, bool nulls, std::optional<duckdb_type> varargs,
              bool volatile_result) {
  LogicalType type(duckdb_create_logical_type(result));
  Register(connection, name, parameters, type.get(), function, nulls, varargs, volatile_result);
}

}  // namespace bigquery_emulator_duckdb::backend_functions
