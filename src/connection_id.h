#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace bigquery_emulator_duckdb {

// The PROJECT.LOCATION.CONNECTION of the connection that `connection_id` names, with the location
// in lower case, or nothing for an ID of another form. EXTERNAL_QUERY accepts
// projects/PROJECT/locations/LOCATION/connections/CONNECTION, PROJECT.LOCATION.CONNECTION and
// LOCATION.CONNECTION, which belongs to `default_project` when it is not empty.
std::optional<std::string> ConnectionKey(std::string_view connection_id,
                                         const std::string& default_project);

}  // namespace bigquery_emulator_duckdb
