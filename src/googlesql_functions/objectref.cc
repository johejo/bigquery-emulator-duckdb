#include "src/googlesql_functions/objectref.h"

#include <algorithm>
#include <functional>
#include <optional>
#include <string>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "nlohmann/json.hpp"
#include "src/connection_id.h"

namespace googlesql_functions {

// bq_obj_authorizer(authorizer, project): OBJ.MAKE_REF's authorizer as PROJECT.LOCATION.CONNECTION,
// with the project of the query when it has none, as BigQuery returns it.
absl::StatusOr<std::string> ObjectRefAuthorizer(const std::string& authorizer,
                                                const std::string& project) {
  const std::optional<std::string> key =
      bigquery_emulator_duckdb::ConnectionKey(authorizer, project);
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
absl::StatusOr<std::string> ReplaceObjectRefAuthorizer(
    const std::string& authorizer, const std::string& project,
    const std::function<bool(const std::string&)>& exists) {
  if (const std::optional<std::string> key =
          bigquery_emulator_duckdb::ConnectionKey(authorizer, "")) {
    return *key;
  }
  if (const std::optional<std::string> key =
          bigquery_emulator_duckdb::ConnectionKey(authorizer, project)) {
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

// bq_obj_details(details): OBJ.MAKE_REF's details, an object that may hold only gcs_metadata and
// errors.
absl::StatusOr<std::string> ObjectRefDetails(std::string text) {
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

}  // namespace googlesql_functions
