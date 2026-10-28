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

#include "google/cloud/bigquery/v2/dataset.pb.h"
#include "google/cloud/bigquery/v2/job.pb.h"
#include "google/cloud/bigquery/v2/routine.pb.h"
#include "google/cloud/bigquery/v2/table.pb.h"
#include "google/protobuf/descriptor.h"
#include "nlohmann/json.hpp"
#include "src/api_error.h"
#include "src/discovery_document.h"
#include "src/httplib.h"

namespace bigquery_emulator_duckdb::server {
namespace {

using google::protobuf::Descriptor;
using google::protobuf::FieldDescriptor;
using nlohmann::json;
namespace v2 = google::cloud::bigquery::v2;

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

// The protocol buffer message of the resource schema `schema`, such as "Table". Its fields give
// the types BigQuery's errors name where the discovery document does not: wrappers, and enums
// declared in other messages, such as Dataset.defaultRoundingMode's TableFieldSchema.RoundingMode.
// Naming the generated messages links their descriptors into the program.
const Descriptor* ProtoMessage(std::string_view schema) {
  static const auto kMessages = std::to_array({
      v2::Dataset::descriptor(),
      v2::Job::descriptor(),
      v2::QueryRequest::descriptor(),
      v2::Routine::descriptor(),
      v2::Table::descriptor(),
  });
  const auto* const message = std::ranges::find_if(
      kMessages, [&](const Descriptor* candidate) { return candidate->name() == schema; });
  if (message != kMessages.end()) {
    return *message;
  }
  throw std::logic_error("No protocol buffer message for the schema " + std::string(schema));
}

// The field of `message` the discovery document names `name`, or null when there is no message
// or it lacks the field, as it does fields newer than its protocol buffers.
const FieldDescriptor* ProtoField(const Descriptor* message, const std::string& name) {
  return message == nullptr ? nullptr : message->FindFieldByCamelcaseName(name);
}

// The value field of the wrapper, such as google.protobuf.Int64Value, that `field` holds, or null
// when it holds none. The discovery document gives a wrapper as a scalar, but BigQuery's errors
// name its value field, such as 'table.expiration_time.value'.
const FieldDescriptor* WrappedValue(const FieldDescriptor* field) {
  const Descriptor* message = field == nullptr ? nullptr : field->message_type();
  if (message == nullptr || message->file()->name() != "google/protobuf/wrappers.proto") {
    return nullptr;
  }
  return message->FindFieldByName("value");
}

std::string Upper(std::string text) {
  std::ranges::transform(text, text.begin(),
                         [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
  return text;
}

// The protocol buffer type of the scalar `parameter` in BigQuery's errors: that of `field`, its
// protocol buffer field, or, when the message lacks it, one the discovery document gives, with
// `enum_type` the full name of an enum, such as "google.cloud.bigquery.v2.Routine.Language".
std::string ProtoType(const QueryParameter& parameter, const FieldDescriptor* field,
                      std::string_view enum_type) {
  if (field != nullptr && field->enum_type() != nullptr) {
    return "type.googleapis.com/" + std::string(field->enum_type()->full_name());
  }
  if (!parameter.values.empty()) {
    return std::format("type.googleapis.com/{}", enum_type);
  }
  if (field != nullptr) {
    return "TYPE_" + Upper(std::string(field->type_name()));
  }
  if (parameter.type == "boolean") {
    return "TYPE_BOOL";
  }
  if (parameter.type == "number") {
    return parameter.format == "float" ? "TYPE_FLOAT" : "TYPE_DOUBLE";
  }
  if (parameter.format.ends_with("int32") || parameter.format.ends_with("int64")) {
    return "TYPE_" + Upper(parameter.format);
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

// The schema of the field `property`, which may refer to another.
const json& Resolved(const json& property) {
  return property.contains("$ref") ? Discovery()["schemas"][property["$ref"].get<std::string>()]
                                   : property;
}

// NOLINTBEGIN(bugprone-easily-swappable-parameters): a value and its schema.
json CheckValue(const json& value, const json& property, const FieldDescriptor* field,
                const ResourceMask& accepted, UnknownFields unknown, const std::string& path,
                const std::string& enum_type);

json CheckObject(const json& value, const json& schema, const Descriptor* message,
                 const ResourceMask& accepted, UnknownFields unknown, Change change,
                 const std::string& path) {
  const json& properties = schema.at("properties");
  // Schemas nested in another, such as those of QueryParameterType.structTypes, have no id.
  const std::string id = schema.value("id", "");
  if (!value.is_object()) {
    throw ApiError::Invalid(
        message == nullptr
            ? std::format("Invalid value at '{}', {}", ProtoPath(path), value.dump())
            : std::format("Invalid value at '{}' (type.googleapis.com/{}), {}", ProtoPath(path),
                          std::string(message->full_name()), value.dump()));
  }
  json result = json::object();
  for (const auto& [key, field] : value.items()) {
    const bool json_name = properties.contains(key);
    const std::string name = json_name ? key : JsonName(key);
    if (!properties.contains(name)) {
      if (unknown == UnknownFields::kIgnore) {
        continue;
      }
      throw ApiError::Invalid(std::format(
          "Invalid JSON payload received. Unknown name \"{}\" at '{}': Cannot find field.", key,
          ProtoPath(path)));
    }
    if (change == Change::kPatch && !json_name) {
      continue;
    }
    const json& property = properties[name];
    const std::string type = Resolved(property).value("type", "");
    // A list for a message or scalar; maps and google.protobuf.Value take other errors.
    if (field.is_array() && type != "array" && type != "any" &&
        (type != "object" || Resolved(property).contains("properties"))) {
      throw ApiError::Invalid(std::format(
          "Invalid JSON payload received. Unknown name \"{}\" at '{}': Proto field is not "
          "repeating, cannot start list.",
          key, ProtoPath(path)));
    }
    const auto child = accepted.fields.find(name);
    const bool handled = accepted.all || child != accepted.fields.end();
    const bool output_only = property.value("readOnly", false);
    if (field.is_null()) {
      if (change != Change::kInsert && handled && !output_only) {
        result[name] = nullptr;
      }
      continue;
    }
    const std::string field_path = std::format("{}.{}", path, name);
    // The enum a field holds is named after it, such as Routine.Language for Routine.language,
    // where its protocol buffer field does not name it.
    std::string type_name = name;
    type_name.front() = static_cast<char>(std::toupper(static_cast<unsigned char>(name.front())));
    const std::string enum_type = std::format("google.cloud.bigquery.v2.{}.{}", id, type_name);
    const ResourceMask everything{.all = true, .fields = {}, .recursive = false};
    const ResourceMask& child_accepted = accepted.all || !handled  ? everything
                                         : child->second.recursive ? accepted
                                                                   : child->second;
    json checked = CheckValue(field, property, ProtoField(message, name), child_accepted, unknown,
                              field_path, enum_type);
    // Output-only fields are checked, as BigQuery does, but ignored, as are the defaults of
    // fields the emulator does not handle, which clients such as bq send.
    if (output_only) {
      continue;
    }
    if (!handled) {
      if (IsDefault(checked, property)) {
        continue;
      }
      throw ApiError::Unsupported(field_path);
    }
    result[name] = std::move(checked);
  }
  return result;
}

json CheckValue(const json& value, const json& property, const FieldDescriptor* field,
                const ResourceMask& accepted, UnknownFields unknown, const std::string& path,
                const std::string& enum_type) {
  const json& schema = Resolved(property);
  const std::string type = schema.value("type", "");
  const Descriptor* message = field == nullptr ? nullptr : field->message_type();
  if (type == "object" && schema.contains("properties")) {
    return CheckObject(value, schema, message, accepted, unknown, Change::kInsert, path);
  }
  // BigQuery takes an object for a repeated message as its only element.
  if (type == "array" && (value.is_array() || (value.is_object() &&
                                               Resolved(schema["items"]).contains("properties")))) {
    const json elements = value.is_array() ? value : json::array({value});
    json result = json::array();
    for (size_t i = 0; i < elements.size(); ++i) {
      result.push_back(CheckValue(elements[i], schema["items"], field, accepted, unknown,
                                  std::format("{}[{}]", path, i), enum_type));
    }
    return result;
  }
  // A map, or a google.protobuf.Struct, whose schema has no properties.
  if (type == "object" && value.is_object()) {
    if (!schema.contains("additionalProperties")) {
      return value;
    }
    const FieldDescriptor* entry =
        field != nullptr && field->is_map() ? message->map_value() : nullptr;
    json result = json::object();
    for (const auto& [key, element] : value.items()) {
      // A null entry is kept: patches remove the label it names.
      result[key] = element.is_null()
                        ? json()
                        : CheckValue(element, schema["additionalProperties"], entry, accepted,
                                     unknown, std::format("{}.{}", path, key), enum_type);
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
    const FieldDescriptor* wrapped = WrappedValue(field);
    throw ApiError::Invalid(std::format(
        "Invalid value at '{}{}' ({}), {}", ProtoPath(path), wrapped == nullptr ? "" : ".value",
        ProtoType(parameter, wrapped == nullptr ? field : wrapped, enum_type), value.dump()));
  }
  throw ApiError::Invalid(std::format("Invalid value at '{}', {}", ProtoPath(path), value.dump()));
}
// NOLINTEND(bugprone-easily-swappable-parameters)

}  // namespace

ResourceMask MaskOf(std::string_view schema, std::initializer_list<std::string_view> paths) {
  ResourceMask mask;
  const json& root = Discovery()["schemas"].at(std::string(schema));
  for (const std::string_view full_path : paths) {
    std::string_view path = full_path;
    const json* object = &root;
    ResourceMask* field = &mask;
    while (true) {
      const size_t dot = path.find('.');
      const std::string name(path.substr(0, dot));
      if (!object->contains("properties") || !(*object)["properties"].contains(name)) {
        throw std::logic_error(std::format("The {} schema has no field {}", schema, full_path));
      }
      const json* child = &Resolved((*object)["properties"][name]);
      if (child->value("type", "") == "array") {
        child = &Resolved((*child)["items"]);
      }
      field = &field->fields[name];
      if (dot == std::string_view::npos) {
        field->all = true;
        field->recursive = child == object;
        break;
      }
      object = child;
      path.remove_prefix(dot + 1);
    }
  }
  return mask;
}

json CheckResource(const json& resource, std::string_view schema, const ResourceMask& accepted,
                   UnknownFields unknown, Change change) {
  // Named in errors like the request field that holds the resource, such as "routine".
  std::string path(schema);
  path.front() = static_cast<char>(std::tolower(static_cast<unsigned char>(path.front())));
  return CheckObject(resource, Discovery()["schemas"].at(std::string(schema)), ProtoMessage(schema),
                     accepted, unknown, change, path);
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
