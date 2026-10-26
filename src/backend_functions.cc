#include "src/backend_functions.h"

#include <optional>
#include <string>

#include "duckdb.h"
#include "src/backend_error.h"
#include "src/duckdb_handle.h"
#include "src/googlesql_aead_duckdb/aead.h"
#include "src/googlesql_functions_duckdb/bignumeric.h"
#include "src/googlesql_functions_duckdb/cast.h"
#include "src/googlesql_functions_duckdb/contains_substr.h"
#include "src/googlesql_functions_duckdb/datetime.h"
#include "src/googlesql_functions_duckdb/interval.h"
#include "src/googlesql_functions_duckdb/json.h"
#include "src/googlesql_functions_duckdb/math.h"
#include "src/googlesql_functions_duckdb/objectref.h"
#include "src/googlesql_functions_duckdb/objectref_options.h"
#include "src/googlesql_functions_duckdb/options.h"
#include "src/googlesql_functions_duckdb/percentile.h"
#include "src/googlesql_functions_duckdb/range.h"
#include "src/googlesql_functions_duckdb/string.h"
#include "src/googlesql_geography/options.h"
#include "src/googlesql_geography_duckdb/geography.h"
#include "src/googlesql_hll_duckdb/hll.h"
#include "src/googlesql_text_analysis_duckdb/text_analysis.h"
#include "src/spanner_query.h"
#include "src/udf/javascript/limits.h"
#include "src/udf/python/options.h"
#include "src/udf_duckdb/javascript.h"
#include "src/udf_duckdb/python.h"

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

}  // namespace

void RegisterBackendFunctions(duckdb_database database,
                              const std::optional<std::string>& session_user,
                              const udf::JavaScriptLimits& javascript,
                              const functions_duckdb::ObjectStorage& storage,
                              const udf::PythonOptions& python,
                              const googlesql_geography::Options& geography,
                              const functions_duckdb::Options& functions) {
  Connection connection;
  if (duckdb_connect(database, connection.out()) == DuckDBError) {
    throw BackendError("DuckDB failed to connect");
  }
  RegisterSessionUser(connection.get(), session_user);
  RegisterSpannerQuery(connection.get());
  functions_duckdb::RegisterBigNumericFunctions(connection.get(), functions);
  functions_duckdb::RegisterMathFunctions(connection.get(), functions);
  functions_duckdb::RegisterStringFunctions(connection.get(), functions);
  text_analysis_duckdb::RegisterTextAnalysisFunctions(connection.get());
  hll_duckdb::RegisterHllCountFunctions(connection.get());
  functions_duckdb::RegisterDatetimeFunctions(connection.get(), functions);
  functions_duckdb::RegisterCastFunctions(connection.get());
  functions_duckdb::RegisterJsonFunctions(connection.get(), functions);
  functions_duckdb::RegisterContainsSubstr(connection.get());
  functions_duckdb::RegisterIntervalFunctions(connection.get());
  functions_duckdb::RegisterRangeFunctions(connection.get());
  // GEOGRAPHY behavior is selected by the caller, above the DuckDB adapter.
  geography_duckdb::RegisterGeographyFunctions(connection.get(), geography);
  aead_duckdb::RegisterAeadFunctions(connection.get());
  functions_duckdb::RegisterPercentileFunctions(connection.get());
  udf_duckdb::RegisterJavaScriptFunctions(connection.get(), javascript);
  udf_duckdb::RegisterPythonFunctions(connection.get(), python);
  functions_duckdb::RegisterObjectRefFunctions(connection.get(), storage);
}

}  // namespace bigquery_emulator_duckdb
