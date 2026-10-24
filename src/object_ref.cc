#include "src/object_ref.h"

#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <system_error>
#include <unordered_map>

#include "absl/strings/escaping.h"
#include "nlohmann/json.hpp"
#include "src/api_error.h"
#include "src/connection_id.h"
#include "src/external_query.h"
#include "src/gcs.h"

namespace bigquery_emulator_duckdb {
namespace {

using nlohmann::json;

std::optional<std::string> StringField(const json& object, const char* name) {
  const auto field = object.find(name);
  if (field == object.end() || !field->is_string()) {
    return std::nullopt;
  }
  return field->get<std::string>();
}

// The message BigQuery records for an authorizer that names no usable connection.
std::string ConnectionError(const std::string& key) {
  return "Connection credential for " + key +
         " cannot be used. Either the connection does not exist, or the user does not have "
         "sufficient permissions (bigquery.objectRefs.read)";
}

}  // namespace

// NOLINTBEGIN(bugprone-easily-swappable-parameters): an ObjectRef and the project of its query.
std::string FetchObjectMetadata(GcsClient& gcs,
                                const std::unordered_map<std::string, std::string>& connections,
                                const std::string& objectref, const std::string& project) {
  // NOLINTEND(bugprone-easily-swappable-parameters)
  const json input = json::parse(objectref);
  // BigQuery keeps the version of an ObjectRef whose fetch fails, and makes a NULL one empty.
  json output = {
      {"uri", input.value("uri", json())},
      {"version", StringField(input, "version").value_or("")},
      {"authorizer", input.value("authorizer", json())},
  };
  const auto fail = [&output](int code, const std::string& message) {
    const json error = {
        {"code", code},
        {"message", message},
        {"source", "OBJ.FETCH_METADATA"},
    };
    output["details"] = {{"errors", json::array({error})}};
    return output.dump();
  };
  if (const std::optional<std::string> authorizer = StringField(input, "authorizer")) {
    const std::optional<std::string> key = ConnectionKey(*authorizer, project);
    if (!key) {
      return fail(400, "Invalid connection: " + *authorizer);
    }
    // BigQuery returns the authorizer with the project it belongs to.
    output["authorizer"] = *key;
    const auto connection = connections.find(*key);
    if (connection == connections.end() || connection->second != kCloudResourceConnection) {
      return fail(400, ConnectionError(*key));
    }
  }
  const std::string uri = StringField(input, "uri").value_or("");
  const size_t slash = uri.find('/', 5);
  if (!uri.starts_with("gs://") || slash == std::string::npos || slash == 5 ||
      slash + 1 == uri.size()) {
    return fail(400, "URI " + uri +
                         " is not in the expected format. Supported format is "
                         "gs://bucket/path/to/object");
  }
  // BigQuery reads the current version of the object whatever version names, once it parses.
  if (const std::optional<std::string> version = StringField(input, "version")) {
    std::int64_t value = 0;
    const auto [end, error] =
        std::from_chars(version->data(), version->data() + version->size(), value);
    if (error != std::errc() || end != version->data() + version->size()) {
      return fail(400, "Failed to parse object version number: " + *version);
    }
  }
  GcsObjectMetadata metadata;
  try {
    metadata = gcs.Metadata(uri);
  } catch (const ApiError& error) {
    return fail(error.http_status(),
                error.http_status() == 404 ? std::string("No such object") : error.what());
  }
  // BigQuery gives the time of the last update in whole seconds, as microseconds.
  const auto updated =
      std::chrono::duration_cast<std::chrono::seconds>(metadata.updated.time_since_epoch());
  json gcs_metadata = {
      {"content_type", metadata.content_type},
      {"size", metadata.size},
      {"updated", updated.count() * 1000000},
  };
  if (std::string md5; absl::Base64Unescape(metadata.md5_hash, &md5) && !md5.empty()) {
    gcs_metadata["md5_hash"] = absl::BytesToHexString(md5);
  }
  output["version"] = std::to_string(metadata.generation);
  output["details"] = {{"gcs_metadata", gcs_metadata}};
  return output.dump();
}

}  // namespace bigquery_emulator_duckdb
