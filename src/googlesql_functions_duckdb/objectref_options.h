#pragma once

#include <functional>
#include <string>

namespace bigquery_emulator_duckdb::functions_duckdb {

// What the OBJ functions reach outside the backend. Either is empty when the backend reaches none.
struct ObjectStorage {
  // OBJ.FETCH_METADATA over the JSON of an ObjectRef, in a query of the project an authorizer
  // without one belongs to, returning the JSON of the ObjectRef it fetched.
  std::function<std::string(const std::string& objectref, const std::string& project)> fetch;
  // Whether the connection PROJECT.LOCATION.CONNECTION exists.
  std::function<bool(const std::string& key)> has_connection;
};

}  // namespace bigquery_emulator_duckdb::functions_duckdb
