#include "src/backend_functions.h"

#include <optional>
#include <string>
#include <vector>

#include "duckdb.h"
#include "src/backend.h"
#include "src/backend_functions/internal.h"
#include "src/duckdb_handle.h"

namespace bigquery_emulator_duckdb {
namespace backend_functions {

void Register(duckdb_connection connection, const char* name,
              const std::vector<duckdb_logical_type>& parameters, duckdb_logical_type result,
              duckdb_scalar_function_t function, bool nulls, std::optional<duckdb_type> varargs) {
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
  if (duckdb_register_scalar_function(connection, scalar.get()) == DuckDBError) {
    throw BackendError(std::string("DuckDB failed to register ") + name);
  }
}

void Register(duckdb_connection connection, const char* name,
              const std::vector<duckdb_type>& parameters, duckdb_logical_type result,
              duckdb_scalar_function_t function, bool nulls, std::optional<duckdb_type> varargs) {
  std::vector<LogicalType> owned;
  std::vector<duckdb_logical_type> types;
  owned.reserve(parameters.size());
  types.reserve(parameters.size());
  for (const duckdb_type parameter : parameters) {
    types.push_back(owned.emplace_back(duckdb_create_logical_type(parameter)).get());
  }
  Register(connection, name, types, result, function, nulls, varargs);
}

void Register(duckdb_connection connection, const char* name,
              const std::vector<duckdb_type>& parameters, duckdb_type result,
              duckdb_scalar_function_t function, bool nulls, std::optional<duckdb_type> varargs) {
  LogicalType type(duckdb_create_logical_type(result));
  Register(connection, name, parameters, type.get(), function, nulls, varargs);
}

}  // namespace backend_functions

void RegisterBackendFunctions(duckdb_database database) {
  Connection connection;
  if (duckdb_connect(database, connection.out()) == DuckDBError) {
    throw BackendError("DuckDB failed to connect");
  }
  backend_functions::RegisterBigNumericFunctions(connection.get());
  backend_functions::RegisterMathFunctions(connection.get());
  backend_functions::RegisterStringFunctions(connection.get());
  backend_functions::RegisterDatetimeFunctions(connection.get());
  backend_functions::RegisterJsonFunctions(connection.get());
}

}  // namespace bigquery_emulator_duckdb
