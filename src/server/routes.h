#pragma once

#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "nlohmann/json_fwd.hpp"

namespace httplib {
struct Request;
}

namespace bigquery_emulator_duckdb::server {

// routes.cc: the methods of the discovery document, which the server routes requests by.

// A query parameter of a method, as the discovery document describes it.
struct QueryParameter {
  std::string type;    // "string", "integer" or "boolean"
  std::string format;  // such as "int32", "uint32" or "uint64"; empty for any value of the type
  std::vector<std::string> values;  // the values an enum takes; empty for any value
};

struct ApiMethod {
  std::string http_method;
  // The cpp-httplib patterns of the method's path below the API prefix, such as
  // "/projects/:projectId/jobs", and of its media upload paths below the root.
  std::string path;
  std::vector<std::string> upload_paths;
  // The query parameters it takes, the API-wide ones such as prettyPrint included.
  std::map<std::string, QueryParameter, std::less<>> parameters;
};

// The parsed discovery document.
const nlohmann::json& Discovery();
// The discovery document's method `id`, such as "bigquery.tables.get". Throws std::logic_error
// when the document lacks it or cpp-httplib cannot match its path.
ApiMethod FindApiMethod(std::string_view id);
// Rejects a request with a query parameter `method` does not take or a value its type does not
// allow, as BigQuery does, and with a non-empty one the emulator does not handle as unsupported.
// The handler of `method` handles the parameters `accepted`, reading or deliberately ignoring
// them; every method handles some of the API-wide ones.
void CheckQueryParameters(const ApiMethod& method, const std::vector<std::string>& accepted,
                          const httplib::Request& request);

}  // namespace bigquery_emulator_duckdb::server
