#include "src/backend_functions.h"

#include <cstdint>
#include <optional>
#include <string>

#include "duckdb.h"
#include "src/backend_error.h"
#include "src/backend_functions/aead.h"
#include "src/backend_functions/bignumeric.h"
#include "src/backend_functions/cast.h"
#include "src/backend_functions/datetime.h"
#include "src/backend_functions/geography.h"
#include "src/backend_functions/hll_count.h"
#include "src/backend_functions/interval.h"
#include "src/backend_functions/javascript.h"
#include "src/backend_functions/javascript_limits.h"
#include "src/backend_functions/json.h"
#include "src/backend_functions/math.h"
#include "src/backend_functions/object_metadata.h"
#include "src/backend_functions/objectref.h"
#include "src/backend_functions/percentile.h"
#include "src/backend_functions/python.h"
#include "src/backend_functions/range.h"
#include "src/backend_functions/string.h"
#include "src/backend_functions/text_analysis.h"
#include "src/duckdb_handle.h"
#include "src/spanner_query.h"

namespace bigquery_emulator_duckdb {

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
  Handle<duckdb_scalar_function, duckdb_destroy_scalar_function> const scalar(
      duckdb_create_scalar_function());
  duckdb_scalar_function_set_name(scalar.get(), "bq_session_user");
  LogicalType const result(duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR));
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

// bq_obj_fetch_metadata(objectref, project) over the JSON text of an ObjectRef. Volatile, since
// the objects it reads change between calls.
void FetchMetadata(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  const auto& fetch = *static_cast<const decltype(backend_functions::ObjectStorage::fetch)*>(
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

void DeleteFetchMetadata(void* data) {
  delete static_cast<decltype(backend_functions::ObjectStorage::fetch)*>(data);
}

void RegisterFetchMetadata(duckdb_connection connection,
                           const decltype(backend_functions::ObjectStorage::fetch)& fetch) {
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
  duckdb_scalar_function_set_extra_info(
      scalar.get(), new decltype(backend_functions::ObjectStorage::fetch)(fetch),
      DeleteFetchMetadata);
  if (duckdb_register_scalar_function(connection, scalar.get()) == DuckDBError) {
    throw BackendError("DuckDB failed to register bq_obj_fetch_metadata");
  }
}

}  // namespace

void RegisterBackendFunctions(duckdb_database database,
                              const std::optional<std::string>& session_user,
                              const backend_functions::JavaScriptLimits& javascript,
                              const backend_functions::ObjectStorage& storage,
                              const backend_functions::PythonOptions& python) {
  Connection connection;
  if (duckdb_connect(database, connection.out()) == DuckDBError) {
    throw BackendError("DuckDB failed to connect");
  }
  RegisterSessionUser(connection.get(), session_user);
  RegisterSpannerQuery(connection.get());
  backend_functions::RegisterBigNumericFunctions(connection.get());
  backend_functions::RegisterMathFunctions(connection.get());
  backend_functions::RegisterStringFunctions(connection.get());
  backend_functions::RegisterTextAnalysisFunctions(connection.get());
  backend_functions::RegisterHllCountFunctions(connection.get());
  backend_functions::RegisterDatetimeFunctions(connection.get());
  backend_functions::RegisterCastFunctions(connection.get());
  backend_functions::RegisterJsonFunctions(connection.get());
  backend_functions::RegisterIntervalFunctions(connection.get());
  backend_functions::RegisterRangeFunctions(connection.get());
  backend_functions::RegisterGeographyFunctions(connection.get());
  backend_functions::RegisterAeadFunctions(connection.get());
  backend_functions::RegisterPercentileFunctions(connection.get());
  backend_functions::RegisterJavaScriptFunctions(connection.get(), javascript);
  backend_functions::RegisterPythonFunctions(connection.get(), python);
  backend_functions::RegisterObjectRefFunctions(connection.get(), storage.has_connection);
  RegisterFetchMetadata(connection.get(), storage.fetch);
}

}  // namespace bigquery_emulator_duckdb
