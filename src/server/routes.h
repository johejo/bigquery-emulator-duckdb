#pragma once

#include <cstdint>
#include <functional>
#include <initializer_list>
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

// A JSON field mask, with whole fields taking precedence over their children.
struct ResourceMask {
  bool all = false;
  std::map<std::string, ResourceMask> fields;
  // Whether this field, of the same schema as the object that holds it, such as
  // TableFieldSchema.fields, takes that object's mask.
  bool recursive = false;
};

// The mask of dot-separated field paths such as "arguments.name". A path may pass through
// repeated fields, which then stand for each of their elements.
ResourceMask MaskOf(std::initializer_list<std::string_view> paths);

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
// What CheckResource does with a field the schema lacks, at any depth. BigQuery rejects one in
// routines.insert, but ignores one in jobs.insert and jobs.query.
enum class UnknownFields : std::uint8_t { kReject, kIgnore };
// How a method writes the resource of its request.
enum class Change : std::uint8_t {
  // It inserts the resource: a null field is one left out.
  kInsert,
  // It replaces a resource, or changes the top-level fields of one the request names. Both keep
  // the top-level fields the request sets to null, which clear them, although BigQuery's update
  // methods keep some fields left out. As BigQuery answers, a patch takes these fields only by
  // their JSON names, ignoring one named as in protocol buffers, which it takes elsewhere.
  kUpdate,
  kPatch,
};

// Checks a request's resource against the discovery document's schema `schema`, such as
// "Routine", and returns it without output-only fields and with the scalars BigQuery converts,
// such as integers given for strings, converted. Null fields are left out, but for the top-level
// ones of an update or patch (`change`) and the entries of maps. Handles a field the schema lacks
// as `unknown` says, rejects a value its type does not allow as invalid, with BigQuery's messages,
// and a field `accepted` leaves out as unsupported. The handler supports `accepted`, which can
// still reject some of their values. Throws std::logic_error when `accepted` names a field the
// schema lacks.
nlohmann::json CheckResource(const nlohmann::json& resource, std::string_view schema,
                             const ResourceMask& accepted,
                             UnknownFields unknown = UnknownFields::kReject,
                             Change change = Change::kInsert);

}  // namespace bigquery_emulator_duckdb::server
