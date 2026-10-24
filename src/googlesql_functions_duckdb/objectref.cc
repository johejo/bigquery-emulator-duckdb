#include "src/googlesql_functions_duckdb/objectref.h"

#include <cstdint>
#include <functional>
#include <string>

#include "duckdb.h"
#include "src/backend_error.h"
#include "src/duckdb_functions/apply.h"
#include "src/duckdb_functions/register.h"
#include "src/duckdb_functions/scalar.h"
#include "src/duckdb_handle.h"
#include "src/googlesql_functions/objectref.h"
#include "src/googlesql_functions_duckdb/objectref_options.h"

namespace bigquery_emulator_duckdb::functions_duckdb {
using namespace duckdb_functions;
namespace {
using googlesql_functions::ReplaceObjectRefAuthorizer;

// bq_obj_replace_authorizer(authorizer, project), over the connections in its extra info.
void ReplaceAuthorizers(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  const auto& exists = *static_cast<const std::function<bool(const std::string&)>*>(
      duckdb_scalar_function_get_extra_info(info));
  EachRow(info, input, output, [&exists](const Arguments& arguments) {
    return ReplaceObjectRefAuthorizer(arguments.String(0), arguments.String(1), exists);
  });
}

void DeleteConnections(void* data) {
  delete static_cast<std::function<bool(const std::string&)>*>(data);
}

// bq_obj_fetch_metadata(objectref, project) over the JSON text of an ObjectRef. Volatile, since
// the objects it reads change between calls.
void FetchMetadata(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  const auto& fetch = *static_cast<const decltype(ObjectStorage::fetch)*>(
      duckdb_scalar_function_get_extra_info(info));
  duckdb_vector objectrefs = duckdb_data_chunk_get_vector(input, 0);
  duckdb_vector projects = duckdb_data_chunk_get_vector(input, 1);
  uint64_t* validity = duckdb_vector_get_validity(objectrefs);
  duckdb_vector_ensure_validity_writable(output);
  for (idx_t row = 0; row < duckdb_data_chunk_get_size(input); ++row) {
    if (validity != nullptr && !duckdb_validity_row_is_valid(validity, row)) {
      duckdb_validity_set_row_invalid(duckdb_vector_get_validity(output), row);
      continue;
    }
    const std::string fetched = fetch(VectorString(objectrefs, row), VectorString(projects, row));
    duckdb_vector_assign_string_element_len(output, row, fetched.data(), fetched.size());
  }
}

void DeleteFetchMetadata(void* data) { delete static_cast<decltype(ObjectStorage::fetch)*>(data); }

void RegisterFetchMetadata(duckdb_connection connection,
                           const decltype(ObjectStorage::fetch)& fetch) {
  if (!fetch) {
    return;
  }
  Handle<duckdb_scalar_function, duckdb_destroy_scalar_function> const scalar(
      duckdb_create_scalar_function());
  duckdb_scalar_function_set_name(scalar.get(), "bq_obj_fetch_metadata");
  LogicalType const json(duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR));
  duckdb_logical_type_set_alias(json.get(), "JSON");
  LogicalType const project(duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR));
  duckdb_scalar_function_add_parameter(scalar.get(), json.get());
  duckdb_scalar_function_add_parameter(scalar.get(), project.get());
  duckdb_scalar_function_set_return_type(scalar.get(), json.get());
  duckdb_scalar_function_set_function(scalar.get(), FetchMetadata);
  duckdb_scalar_function_set_special_handling(scalar.get());
  duckdb_scalar_function_set_volatile(scalar.get());
  duckdb_scalar_function_set_extra_info(scalar.get(), new decltype(ObjectStorage::fetch)(fetch),
                                        DeleteFetchMetadata);
  if (duckdb_register_scalar_function(connection, scalar.get()) == DuckDBError) {
    throw BackendError("DuckDB failed to register bq_obj_fetch_metadata");
  }
}

}  // namespace

void RegisterObjectRefFunctions(duckdb_connection connection, const ObjectStorage& storage) {
  Register(connection, "bq_obj_authorizer", {kVarchar, kVarchar}, kVarchar,
           Apply<googlesql_functions::ObjectRefAuthorizer>);
  Register(connection, "bq_obj_details", {kVarchar}, kVarchar,
           Apply<googlesql_functions::ObjectRefDetails>);
  Handle<duckdb_scalar_function, duckdb_destroy_scalar_function> const scalar(
      duckdb_create_scalar_function());
  duckdb_scalar_function_set_name(scalar.get(), "bq_obj_replace_authorizer");
  LogicalType const varchar(duckdb_create_logical_type(kVarchar));
  duckdb_scalar_function_add_parameter(scalar.get(), varchar.get());
  duckdb_scalar_function_add_parameter(scalar.get(), varchar.get());
  duckdb_scalar_function_set_return_type(scalar.get(), varchar.get());
  duckdb_scalar_function_set_function(scalar.get(), ReplaceAuthorizers);
  duckdb_scalar_function_set_extra_info(
      scalar.get(), new std::function<bool(const std::string&)>(storage.has_connection),
      DeleteConnections);
  if (duckdb_register_scalar_function(connection, scalar.get()) == DuckDBError) {
    throw BackendError("DuckDB failed to register bq_obj_replace_authorizer");
  }
  RegisterFetchMetadata(connection, storage.fetch);
}

}  // namespace bigquery_emulator_duckdb::functions_duckdb
