#pragma once

#include <optional>
#include <vector>

#include "duckdb.h"

namespace bigquery_emulator_duckdb::duckdb_functions {

// Registers `function` under `name`, taking `parameters` and returning `result`. Unless `nulls`
// is false, DuckDB makes the result NULL for a NULL argument without calling `function`. A
// `volatile_result` keeps DuckDB from folding or sharing calls with the same arguments.
// DuckDB owns extra_info and calls destroy_extra_info when the registration is released.
// Throws BackendError when DuckDB rejects it.
void Register(duckdb_connection connection, const char* name,
              const std::vector<duckdb_logical_type>& parameters, duckdb_logical_type result,
              duckdb_scalar_function_t function, bool nulls = true,
              std::optional<duckdb_type> varargs = std::nullopt, bool volatile_result = false,
              void* extra_info = nullptr, duckdb_delete_callback_t destroy_extra_info = nullptr);
void Register(duckdb_connection connection, const char* name,
              const std::vector<duckdb_type>& parameters, duckdb_logical_type result,
              duckdb_scalar_function_t function, bool nulls = true,
              std::optional<duckdb_type> varargs = std::nullopt, bool volatile_result = false,
              void* extra_info = nullptr, duckdb_delete_callback_t destroy_extra_info = nullptr);
void Register(duckdb_connection connection, const char* name,
              const std::vector<duckdb_type>& parameters, duckdb_type result,
              duckdb_scalar_function_t function, bool nulls = true,
              std::optional<duckdb_type> varargs = std::nullopt, bool volatile_result = false,
              void* extra_info = nullptr, duckdb_delete_callback_t destroy_extra_info = nullptr);

namespace register_internal {

template <typename Context>
void Destroy(void* context) {
  delete static_cast<Context*>(context);
}

}  // namespace register_internal

// As Register, with a copy of `context` that the registration owns and FunctionContext reads:
// a choice the caller makes once, rather than an argument of each call.
template <typename Context, typename Result>
void RegisterWithContext(duckdb_connection connection, const char* name,
                         const std::vector<duckdb_type>& parameters, Result result,
                         duckdb_scalar_function_t function, const Context& context,
                         bool nulls = true, std::optional<duckdb_type> varargs = std::nullopt) {
  Register(connection, name, parameters, result, function, nulls, varargs,
           /*volatile_result=*/false, new Context(context), register_internal::Destroy<Context>);
}
template <typename Context>
void RegisterWithContext(duckdb_connection connection, const char* name,
                         const std::vector<duckdb_logical_type>& parameters,
                         duckdb_logical_type result, duckdb_scalar_function_t function,
                         const Context& context, bool nulls = true,
                         std::optional<duckdb_type> varargs = std::nullopt) {
  Register(connection, name, parameters, result, function, nulls, varargs,
           /*volatile_result=*/false, new Context(context), register_internal::Destroy<Context>);
}

// The context that RegisterWithContext gave the function being called.
template <typename Context>
const Context& FunctionContext(duckdb_function_info info) {
  return *static_cast<const Context*>(duckdb_scalar_function_get_extra_info(info));
}

}  // namespace bigquery_emulator_duckdb::duckdb_functions
