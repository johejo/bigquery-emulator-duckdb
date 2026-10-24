#include "src/backend_functions/objectref.h"

#include <algorithm>
#include <functional>
#include <optional>
#include <string>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "duckdb.h"
#include "nlohmann/json.hpp"
#include "src/backend_error.h"
#include "src/duckdb_functions/register.h"
#include "src/duckdb_functions/scalar.h"
#include "src/duckdb_handle.h"
#include "src/external_query.h"

namespace bigquery_emulator_duckdb::backend_functions {
using namespace duckdb_functions;
namespace {

// bq_obj_authorizer(authorizer, project): OBJ.MAKE_REF's authorizer as PROJECT.LOCATION.CONNECTION,
// with the project of the query when it has none, as BigQuery returns it.
absl::StatusOr<std::string> Authorizer(const Arguments& arguments) {
  const std::string authorizer = arguments.String(0);
  const std::optional<std::string> key = ConnectionKey(authorizer, arguments.String(1));
  if (!key) {
    return absl::InvalidArgumentError(
        "Connection id " + authorizer +
        " is not in the expected format. Supported formats are "
        "projects/{project}/locations/{location}/connections/{connection} or "
        "{project}.{location}.{connection} or {location}.{connection}; error in OBJ.MAKE_REF "
        "expression");
  }
  return *key;
}

// The authorizer that OBJ.MAKE_REF puts in place of an ObjectRef's, as PROJECT.LOCATION.CONNECTION.
// BigQuery takes one without a project only when the connection exists, and does not check one
// with a project.
absl::StatusOr<std::string> ReplaceAuthorizer(
    const std::string& authorizer, const std::string& project,
    const std::function<bool(const std::string&)>& exists) {
  if (const std::optional<std::string> key = ConnectionKey(authorizer, "")) {
    return *key;
  }
  if (const std::optional<std::string> key = ConnectionKey(authorizer, project)) {
    if (exists && exists(*key)) {
      return *key;
    }
    return absl::InvalidArgumentError("Connection '" + authorizer + "' cannot be found.");
  }
  return absl::InvalidArgumentError(
      "Connection id " + authorizer +
      " is not in the expected format. Supported formats are "
      "projects/{project}/locations/{location}/connections/{connection} or "
      "{project}.{location}.{connection} or {location}.{connection}");
}

// bq_obj_replace_authorizer(authorizer, project), over the connections in its extra info.
void ReplaceAuthorizers(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  const auto& exists = *static_cast<const std::function<bool(const std::string&)>*>(
      duckdb_scalar_function_get_extra_info(info));
  EachRow(info, input, output, [&exists](const Arguments& arguments) {
    return ReplaceAuthorizer(arguments.String(0), arguments.String(1), exists);
  });
}

void DeleteConnections(void* data) {
  delete static_cast<std::function<bool(const std::string&)>*>(data);
}

// bq_obj_details(details): OBJ.MAKE_REF's details, an object that may hold only gcs_metadata and
// errors.
absl::StatusOr<std::string> Details(const Arguments& arguments) {
  std::string text = arguments.String(0);
  const nlohmann::json details = nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
  const auto valid = [&details] {
    return details.is_object() && std::ranges::all_of(details.items(), [](const auto& item) {
             return item.key() == "gcs_metadata" || item.key() == "errors";
           });
  };
  if (!valid()) {
    return absl::InvalidArgumentError(
        "Invalid field in objectref details, only a JSON object named gcs_metadata or errors is "
        "allowed; error in OBJ.MAKE_REF expression");
  }
  return text;
}

}  // namespace

void RegisterObjectRefFunctions(duckdb_connection connection,
                                const std::function<bool(const std::string&)>& has_connection) {
  Register(connection, "bq_obj_authorizer", {kVarchar, kVarchar}, kVarchar, EachRow<Authorizer>);
  Register(connection, "bq_obj_details", {kVarchar}, kVarchar, EachRow<Details>);
  Handle<duckdb_scalar_function, duckdb_destroy_scalar_function> const scalar(
      duckdb_create_scalar_function());
  duckdb_scalar_function_set_name(scalar.get(), "bq_obj_replace_authorizer");
  LogicalType const varchar(duckdb_create_logical_type(kVarchar));
  duckdb_scalar_function_add_parameter(scalar.get(), varchar.get());
  duckdb_scalar_function_add_parameter(scalar.get(), varchar.get());
  duckdb_scalar_function_set_return_type(scalar.get(), varchar.get());
  duckdb_scalar_function_set_function(scalar.get(), ReplaceAuthorizers);
  duckdb_scalar_function_set_extra_info(
      scalar.get(), new std::function<bool(const std::string&)>(has_connection), DeleteConnections);
  if (duckdb_register_scalar_function(connection, scalar.get()) == DuckDBError) {
    throw BackendError("DuckDB failed to register bq_obj_replace_authorizer");
  }
}

}  // namespace bigquery_emulator_duckdb::backend_functions
