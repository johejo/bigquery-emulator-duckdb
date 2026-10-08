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

}  // namespace backend_functions

namespace {

// Keep the call in stored views so that reopening the database uses the current identity.
void SessionUser(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  const auto& user =
      *static_cast<const std::optional<std::string>*>(duckdb_scalar_function_get_extra_info(info));
  if (!user) {
    duckdb_scalar_function_set_error(info, "The emulator does not support function SESSION_USER");
    return;
  }
  for (idx_t row = 0; row < duckdb_data_chunk_get_size(input); ++row) {
    duckdb_vector_assign_string_element_len(output, row, user->data(), user->size());
  }
}

void BindSessionUser(duckdb_bind_info info) {
  const auto& user = *static_cast<const std::optional<std::string>*>(
      duckdb_scalar_function_bind_get_extra_info(info));
  if (!user) {
    duckdb_scalar_function_bind_set_error(info,
                                          "The emulator does not support function SESSION_USER");
  }
}

void DeleteSessionUser(void* data) { delete static_cast<std::optional<std::string>*>(data); }

void RegisterSessionUser(duckdb_connection connection,
                         const std::optional<std::string>& session_user) {
  Handle<duckdb_scalar_function, duckdb_destroy_scalar_function> scalar(
      duckdb_create_scalar_function());
  duckdb_scalar_function_set_name(scalar.get(), "bq_session_user");
  LogicalType result(duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR));
  duckdb_scalar_function_set_return_type(scalar.get(), result.get());
  duckdb_scalar_function_set_bind(scalar.get(), BindSessionUser);
  duckdb_scalar_function_set_function(scalar.get(), SessionUser);
  duckdb_scalar_function_set_volatile(scalar.get());
  duckdb_scalar_function_set_extra_info(scalar.get(), new std::optional<std::string>(session_user),
                                        DeleteSessionUser);
  if (duckdb_register_scalar_function(connection, scalar.get()) == DuckDBError) {
    throw BackendError("DuckDB failed to register bq_session_user");
  }
}

}  // namespace

void RegisterBackendFunctions(duckdb_database database,
                              const std::optional<std::string>& session_user) {
  Connection connection;
  if (duckdb_connect(database, connection.out()) == DuckDBError) {
    throw BackendError("DuckDB failed to connect");
  }
  RegisterSessionUser(connection.get(), session_user);
  backend_functions::RegisterBigNumericFunctions(connection.get());
  backend_functions::RegisterMathFunctions(connection.get());
  backend_functions::RegisterStringFunctions(connection.get());
  backend_functions::RegisterDatetimeFunctions(connection.get());
  backend_functions::RegisterJsonFunctions(connection.get());
  backend_functions::RegisterAeadFunctions(connection.get());
  backend_functions::RegisterPercentileFunctions(connection.get());
}

}  // namespace bigquery_emulator_duckdb
