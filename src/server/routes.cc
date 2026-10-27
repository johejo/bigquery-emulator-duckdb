#include "src/server/routes.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <format>
#include <initializer_list>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
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

// The name of a field path such as "routine.routineReference" in BigQuery's errors, which spell
// fields as in protocol buffers: "routine.routine_reference".
std::string ProtoPath(std::string_view path) {
  std::string result;
  for (const char c : path) {
    if (std::isupper(static_cast<unsigned char>(c)) != 0) {
      result += '_';
      result += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    } else {
      result += c;
    }
  }
  return result;
}

// The JSON name of the protocol buffer field `name`, such as "enableListInference" for
// "enable_list_inference". BigQuery takes fields by either name, as protocol buffers' JSON
// parsers do; bq sends parquetOptions.enable_list_inference.
std::string JsonName(std::string_view name) {
  std::string result;
  bool upper = false;
  for (const char c : name) {
    if (c == '_') {
      upper = true;
    } else {
      result += upper ? static_cast<char>(std::toupper(static_cast<unsigned char>(c))) : c;
      upper = false;
    }
  }
  return result;
}

// The protocol buffer type of the scalar `parameter` in BigQuery's errors. `enum_type` is the
// enum's full name, such as "google.cloud.bigquery.v2.Routine.Language".
std::string ProtoType(const QueryParameter& parameter, std::string_view enum_type) {
  if (!parameter.values.empty()) {
    return std::format("type.googleapis.com/{}", enum_type);
  }
  if (parameter.type == "boolean") {
    return "TYPE_BOOL";
  }
  if (parameter.type == "number") {
    return parameter.format == "float" ? "TYPE_FLOAT" : "TYPE_DOUBLE";
  }
  if (parameter.format.ends_with("int32") || parameter.format.ends_with("int64")) {
    std::string type = "TYPE_" + parameter.format;
    std::ranges::transform(type, type.begin(),
                           [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return type;
  }
  return parameter.format == "byte" ? "TYPE_BYTES" : "TYPE_STRING";
}

// `value` as a value of the scalar `parameter`, or nothing when it is not one. BigQuery parses
// JSON as protocol buffers do, which also take integers as strings and 64-bit integers, which
// are strings, as numbers; it also takes integers as strings and "true" and "false" as booleans.
std::optional<json> ScalarValue(const QueryParameter& parameter, const json& value) {
  if (parameter.type == "boolean") {
    if (value.is_boolean()) {
      return value;
    }
    if (value == "true" || value == "false") {
      return value == "true";
    }
    return std::nullopt;
  }
  if (parameter.type == "number") {
    if (!value.is_string()) {
      return value.is_number() ? std::optional(value) : std::nullopt;
    }
    const auto& text = value.get_ref<const std::string&>();
    double number = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), number);
    if (text == "NaN" || text == "Infinity" || text == "-Infinity" ||
        (error == std::errc() && end == text.data() + text.size())) {
      return value;
    }
    return std::nullopt;
  }
  if (value.is_number_integer() && parameter.values.empty()) {
    return Fits(parameter, value.dump())
               ? std::optional(parameter.type == "integer" ? value : json(value.dump()))
               : std::nullopt;
  }
  if (value.is_string() && Fits(parameter, value.get_ref<const std::string&>())) {
    return parameter.type == "integer" ? json::parse(value.get_ref<const std::string&>()) : value;
  }
  return std::nullopt;
}

// Whether `value` of the field `property` is its default, which BigQuery cannot tell from
// leaving the field out. A message never is: an empty one is set.
bool IsDefault(const json& value, const json& property) {
  if (property.contains("$ref")) {
    return false;
  }
  if (property.contains("default")) {
    const auto& text = property["default"].get_ref<const std::string&>();
    return value.is_string() ? value == text : value.dump() == text;
  }
  if (value.is_string()) {
    return value.get_ref<const std::string&>().empty() ||
           (value == "0" && property.value("format", "").ends_with("int64"));
  }
  return value == false || value == 0 || ((value.is_array() || value.is_object()) && value.empty());
}

// NOLINTBEGIN(bugprone-easily-swappable-parameters): a value and its schema.
json CheckValue(const json& value, const json& property, const ResourceMask& accepted,
                UnknownFields unknown, const std::string& path, const std::string& enum_type);

json CheckObject(const json& value, const json& schema, const ResourceMask& accepted,
                 UnknownFields unknown, const std::string& path) {
  const json& properties = schema.at("properties");
  // Schemas nested in another, such as those of QueryParameterType.structTypes, have no id.
  const std::string id = schema.value("id", "");
  for (const auto& [name, child] : accepted.fields) {
    if (!properties.contains(name)) {
      throw std::logic_error(std::format("The {} schema has no field {}", id, name));
    }
  }
  if (!value.is_object()) {
    throw ApiError::Invalid(
        std::format("Invalid value at '{}', {}", ProtoPath(path), value.dump()));
  }
  json result = json::object();
  for (const auto& [key, field] : value.items()) {
    const std::string name = properties.contains(key) ? key : JsonName(key);
    if (!properties.contains(name)) {
      if (unknown == UnknownFields::kIgnore) {
        continue;
      }
      throw ApiError::Invalid(std::format(
          "Invalid JSON payload received. Unknown name \"{}\" at '{}': Cannot find field.", key,
          ProtoPath(path)));
    }
    if (field.is_null()) {
      continue;
    }
    const std::string field_path = std::format("{}.{}", path, name);
    // The enum a field holds is named after it, such as Routine.Language for Routine.language.
    std::string type_name = name;
    type_name.front() = static_cast<char>(std::toupper(static_cast<unsigned char>(name.front())));
    const std::string enum_type = std::format("google.cloud.bigquery.v2.{}.{}", id, type_name);
    const auto child = accepted.fields.find(name);
    const bool handled = accepted.all || child != accepted.fields.end();
    const ResourceMask everything{.all = true, .fields = {}};
    json checked =
        CheckValue(field, properties[name], accepted.all || !handled ? everything : child->second,
                   unknown, field_path, enum_type);
    // Output-only fields are checked, as BigQuery does, but ignored, as are the defaults of
    // fields the emulator does not handle, which clients such as bq send.
    if (properties[name].value("readOnly", false)) {
      continue;
    }
    if (!handled) {
      if (IsDefault(checked, properties[name])) {
        continue;
      }
      throw ApiError::Unsupported(field_path);
    }
    result[name] = std::move(checked);
  }
  return result;
}

json CheckValue(const json& value, const json& property, const ResourceMask& accepted,
                UnknownFields unknown, const std::string& path, const std::string& enum_type) {
  const json& schema = property.contains("$ref")
                           ? Discovery()["schemas"][property["$ref"].get<std::string>()]
                           : property;
  const std::string type = schema.value("type", "");
  if (type == "object" && schema.contains("properties")) {
    return CheckObject(value, schema, accepted, unknown, path);
  }
  if (type == "array" && value.is_array()) {
    json result = json::array();
    for (size_t i = 0; i < value.size(); ++i) {
      result.push_back(CheckValue(value[i], schema["items"], accepted, unknown,
                                  std::format("{}[{}]", path, i), enum_type));
    }
    return result;
  }
  // A map, or a google.protobuf.Struct, whose schema has no properties.
  if (type == "object" && value.is_object()) {
    if (!schema.contains("additionalProperties")) {
      return value;
    }
    json result = json::object();
    for (const auto& [key, entry] : value.items()) {
      result[key] = CheckValue(entry, schema["additionalProperties"], accepted, unknown,
                               std::format("{}.{}", path, key), enum_type);
    }
    return result;
  }
  if (type == "any") {
    return value;
  }
  if (type != "array" && type != "object") {
    const QueryParameter parameter = ReadQueryParameter(schema);
    if (std::optional<json> scalar = ScalarValue(parameter, value); scalar.has_value()) {
      return *std::move(scalar);
    }
    throw ApiError::Invalid(std::format("Invalid value at '{}' ({}), {}", ProtoPath(path),
                                        ProtoType(parameter, enum_type), value.dump()));
  }
  throw ApiError::Invalid(std::format("Invalid value at '{}', {}", ProtoPath(path), value.dump()));
}
// NOLINTEND(bugprone-easily-swappable-parameters)

}  // namespace

ResourceMask MaskOf(std::initializer_list<std::string_view> paths) {
  ResourceMask mask;
  for (std::string_view path : paths) {
    ResourceMask* field = &mask;
    while (true) {
      const size_t dot = path.find('.');
      field = &field->fields[std::string(path.substr(0, dot))];
      if (dot == std::string_view::npos) {
        break;
      }
      path.remove_prefix(dot + 1);
    }
    field->all = true;
  }
  return mask;
}

json CheckResource(const json& resource, std::string_view schema, const ResourceMask& accepted,
                   UnknownFields unknown) {
  // Named in errors like the request field that holds the resource, such as "routine".
  std::string path(schema);
  path.front() = static_cast<char>(std::tolower(static_cast<unsigned char>(path.front())));
  return CheckObject(resource, Discovery()["schemas"].at(std::string(schema)), accepted, unknown,
                     path);
}

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
      throw ApiError::Unsupported(name);
    }
    if (name == "alt" && value != "json") {
      throw ApiError::Unsupported("alt=" + value);
    }
  }
}

}  // namespace bigquery_emulator_duckdb::server
