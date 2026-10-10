#include "src/server/routes.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "nlohmann/json.hpp"
#include "src/api_error.h"
#include "src/discovery_document.h"
#include "src/httplib.h"

namespace bigquery_emulator_duckdb::server {
namespace {

using nlohmann::json;

// The API-wide parameters every method accepts. The others are unsupported: `callback` wraps the
// response in JSONP and `$.xgafv` changes the error format.
constexpr std::array<std::string_view, 9> kAcceptedApiParameters = {
    // Credentials and quota accounting, which the emulator does not check.
    "access_token",
    "key",
    "oauth_token",
    "quotaUser",
    // Formatting of the JSON response.
    "prettyPrint",
    // Partial responses are not emulated: the response carries every field, a superset of the
    // fields asked for. Clients such as the Go client ask for them on every jobs.get.
    "fields",
    // The response format; anything but JSON is rejected below.
    "alt",
    // Media uploads, which jobs.insert reads.
    "uploadType",
    "upload_protocol",
};

QueryParameter ReadQueryParameter(const json& parameter) {
  QueryParameter result{
      .type = parameter.value("type", ""),
      .format = parameter.value("format", ""),
      .values = {},
  };
  if (parameter.contains("enum")) {
    result.values = parameter["enum"].get<std::vector<std::string>>();
  }
  return result;
}

// The cpp-httplib pattern of a discovery path template such as "projects/{+projectId}/jobs":
// "/projects/:projectId/jobs". cpp-httplib matches a path parameter to a whole segment, which is
// what the pattern of every path parameter of the discovery document asks for.
std::string RoutePattern(std::string_view path) {
  std::string pattern;
  if (path.starts_with('/')) {
    path.remove_prefix(1);
  }
  while (true) {
    const size_t slash = path.find('/');
    std::string_view segment = path.substr(0, slash);
    pattern += '/';
    if (segment.starts_with('{') && segment.ends_with('}')) {
      segment = segment.substr(1, segment.size() - 2);
      if (segment.starts_with('+')) {
        segment.remove_prefix(1);
      }
      pattern += ':';
      pattern += segment;
    } else if (segment.find_first_of("{}:") != std::string_view::npos) {
      // Such as "{+datasetId}:undelete", which cpp-httplib's path parameters cannot match.
      throw std::logic_error("Cannot route " + std::string(path));
    } else {
      pattern += segment;
    }
    if (slash == std::string_view::npos) {
      return pattern;
    }
    path.remove_prefix(slash + 1);
  }
}

// Whether `text` is an integer of type T in [minimum, maximum].
template <typename T>
bool IsInteger(std::string_view text, T minimum, T maximum) {
  T value{};
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
  return error == std::errc() && end == text.data() + text.size() && value >= minimum &&
         value <= maximum;
}

bool Fits(const QueryParameter& parameter, std::string_view value) {
  if (!parameter.values.empty()) {
    return std::ranges::find(parameter.values, value) != parameter.values.end();
  }
  if (parameter.type == "boolean") {
    return value == "true" || value == "false";
  }
  if (parameter.format == "int32") {
    return IsInteger<int64_t>(value, std::numeric_limits<int32_t>::min(),
                              std::numeric_limits<int32_t>::max());
  }
  if (parameter.format == "uint32") {
    return IsInteger<uint64_t>(value, 0, std::numeric_limits<uint32_t>::max());
  }
  if (parameter.format == "int64") {
    return IsInteger<int64_t>(value, std::numeric_limits<int64_t>::min(),
                              std::numeric_limits<int64_t>::max());
  }
  if (parameter.format == "uint64") {
    return IsInteger<uint64_t>(value, 0, std::numeric_limits<uint64_t>::max());
  }
  return true;
}

}  // namespace

const json& Discovery() {
  static const json* const kDocument = new json(json::parse(DiscoveryDocument()));
  return *kDocument;
}

ApiMethod FindApiMethod(std::string_view id) {
  for (const auto& [resource_name, resource] : Discovery()["resources"].items()) {
    for (const auto& [method_name, method] : resource["methods"].items()) {
      if (method["id"] != id) {
        continue;
      }
      ApiMethod result{
          .http_method = method["httpMethod"].get<std::string>(),
          .path = RoutePattern(method["path"].get<std::string>()),
          .upload_paths = {},
          .parameters = {},
      };
      if (method.contains("mediaUpload")) {
        for (const auto& [protocol, upload] : method["mediaUpload"]["protocols"].items()) {
          result.upload_paths.push_back(RoutePattern(upload["path"].get<std::string>()));
        }
      }
      const json parameters = method.value("parameters", json::object());
      for (const auto& [name, parameter] : parameters.items()) {
        if (parameter["location"] == "query") {
          result.parameters.emplace(name, ReadQueryParameter(parameter));
        } else if (parameter.value("pattern", "^[^/]+$") != "^[^/]+$") {
          throw std::logic_error("Cannot route path parameter " + name + " of " + std::string(id));
        }
      }
      for (const auto& [name, parameter] : Discovery()["parameters"].items()) {
        result.parameters.emplace(name, ReadQueryParameter(parameter));
      }
      return result;
    }
  }
  throw std::logic_error("The discovery document has no method " + std::string(id));
}

void CheckQueryParameters(const ApiMethod& method, const std::vector<std::string>& accepted,
                          const httplib::Request& request) {
  for (const auto& [name, value] : request.params) {
    const auto parameter = method.parameters.find(name);
    if (parameter == method.parameters.end()) {
      throw ApiError::Invalid(std::format(
          "Unknown name \"{0}\": Cannot bind query parameter. Field '{0}' could not be found in "
          "request message.",
          name));
    }
    // An empty value leaves the parameter unset.
    if (value.empty()) {
      continue;
    }
    if (!Fits(parameter->second, value)) {
      throw ApiError::Invalid(std::format("Invalid value for {}: \"{}\"", name, value));
    }
    if (std::ranges::find(accepted, name) == accepted.end() &&
        std::ranges::find(kAcceptedApiParameters, name) == kAcceptedApiParameters.end()) {
      throw ApiError::Invalid("The emulator does not support " + name);
    }
    if (name == "alt" && value != "json") {
      throw ApiError::Invalid("The emulator does not support alt=" + value);
    }
  }
}

}  // namespace bigquery_emulator_duckdb::server
