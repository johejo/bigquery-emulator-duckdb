#include "src/server/requests.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "nlohmann/json.hpp"
#include "src/api_error.h"
#include "src/emulator.h"
#include "src/field_schema.h"
#include "src/httplib.h"
#include "src/query_parameters.h"
#include "src/references.h"
#include "src/routine.h"
#include "src/server/routes.h"
#include "src/table_metadata.h"

namespace bigquery_emulator_duckdb::server {
namespace {

using nlohmann::json;

int64_t QueryParamInt(const httplib::Request& request, const char* name, int64_t fallback) {
  if (!request.has_param(name)) {
    return fallback;
  }
  try {
    const std::string value = request.get_param_value(name);
    if (value.empty() && std::string_view(name) == "pageToken") {
      return fallback;
    }
    return std::stoll(value);
  } catch (const std::exception&) {
    throw ApiError::Invalid(std::string("Invalid value for ") + name);
  }
}

// The string `object[key]`, or "" when it is missing.
std::string StringField(const json& object, const char* key, std::string_view what) {
  if (!object.contains(key)) return "";
  if (!object[key].is_string()) throw ApiError::Invalid("Invalid " + std::string(what));
  return object[key].get<std::string>();
}

// Reads a TableReference, whose project defaults to the job's. `what` names it in errors.
TableReference ParseTableReference(const json& table, std::string_view what) {
  if (!table.is_object()) throw ApiError::Invalid("Invalid " + std::string(what));
  TableReference result{
      .project_id = StringField(table, "projectId", what),
      .dataset_id = StringField(table, "datasetId", what),
      .table_id = StringField(table, "tableId", what),
  };
  if (result.dataset_id.empty() || result.table_id.empty()) {
    throw ApiError::Invalid("Invalid " + std::string(what));
  }
  return result;
}

std::optional<DatasetReference> ParseDefaultDataset(const json& config) {
  if (!config.contains("defaultDataset")) {
    return std::nullopt;
  }
  const json& dataset = config["defaultDataset"];
  if (!dataset.is_object()) throw ApiError::Invalid("Invalid default dataset");
  return DatasetReference{
      .project_id = StringField(dataset, "projectId", "default dataset"),
      .dataset_id = StringField(dataset, "datasetId", "default dataset"),
  };
}

// jobs.query takes the query configuration as the request body and jobs.insert takes it as
// configuration.query, but the fields this emulator reads are spelled the same in both.
QueryRequest ParseQueryConfiguration(const std::string& project_id, const json& config) {
  if (!config.contains("query") || !config["query"].is_string()) {
    throw ApiError::Invalid("Required parameter is missing: query");
  }
  // Only GoogleSQL is emulated. An omitted useLegacySql runs as GoogleSQL, although BigQuery
  // defaults it to true.
  if (config.contains("useLegacySql") && config["useLegacySql"] == true) {
    throw ApiError::Invalid("The emulator does not support legacy SQL; set useLegacySql to false");
  }
  QueryRequest request;
  request.project_id = project_id;
  request.query = config["query"].get<std::string>();
  request.default_dataset = ParseDefaultDataset(config);
  request.parameters = QueryParameters::Parse(config.value("queryParameters", json::array()));
  return request;
}

// Reads the dispositions in a job configuration into `job`, which keeps its job type's default
// for any the configuration omits.
template <typename T>
void ParseDispositions(const json& config, T& job) {
  if (config.contains("createDisposition")) {
    const std::string name = StringField(config, "createDisposition", "create disposition");
    const std::optional<CreateDisposition> disposition = ParseCreateDisposition(name);
    if (!disposition.has_value()) throw ApiError::Invalid("Invalid create disposition: " + name);
    job.create_disposition = *disposition;
  }
  if (config.contains("writeDisposition")) {
    const std::string name = StringField(config, "writeDisposition", "write disposition");
    const std::optional<WriteDisposition> disposition = ParseWriteDisposition(name);
    if (!disposition.has_value()) throw ApiError::Invalid("Invalid write disposition: " + name);
    job.write_disposition = *disposition;
  }
}

std::string JobId(const json& body) {
  const json reference = body.value("jobReference", json::object());
  if (!reference.is_object()) throw ApiError::Invalid("Invalid job reference");
  return StringField(reference, "jobId", "job ID");
}

// Query and load jobs that create their destination table could also partition and cluster it,
// which the emulator does not support yet.
void RejectDestinationLayout(const json& config) {
  for (const char* key : {"timePartitioning", "rangePartitioning", "clustering"}) {
    if (config.contains(key) && !config[key].is_null()) {
      throw ApiError::Invalid(std::string("The emulator does not support ") + key +
                              " for a destination table");
    }
  }
}

json Configuration(const json& body) {
  const json config = body.value("configuration", json::object());
  if (!config.is_object()) throw ApiError::Invalid("Invalid job configuration");
  return config;
}

QueryRequest ParseQueryJob(const std::string& project_id, const json& body) {
  const json config = Configuration(body);
  const json& query = config["query"];
  if (!query.is_object()) throw ApiError::Invalid("Invalid query configuration");
  QueryRequest request = ParseQueryConfiguration(project_id, query);
  request.job_id = JobId(body);
  request.dry_run = config.value("dryRun", false);
  if (query.contains("destinationTable")) {
    request.destination_table = ParseTableReference(query["destinationTable"], "destination table");
  }
  ParseDispositions(query, request);
  RejectDestinationLayout(query);
  return request;
}

LoadRequest ParseLoadJob(const std::string& project_id, const json& body) {
  const json config = Configuration(body);
  const json& load = config["load"];
  if (!load.is_object() || !load.contains("destinationTable")) {
    throw ApiError::Invalid("Invalid destination table");
  }
  LoadRequest request;
  request.project_id = project_id;
  request.job_id = JobId(body);
  request.load.destination_table =
      ParseTableReference(load["destinationTable"], "destination table");
  ParseDispositions(load, request.load);
  RejectDestinationLayout(load);
  request.load.configuration = load;
  return request;
}

CopyRequest ParseCopyJob(const std::string& project_id, const json& body) {
  const json config = Configuration(body);
  const json& copy = config["copy"];
  if (!copy.is_object() || !copy.contains("destinationTable")) {
    throw ApiError::Invalid("Invalid destination table");
  }
  // A snapshot, a restore and a clone record where they came from, which a copy does not.
  if (const json operation = copy.value("operationType", json());
      !operation.is_null() && operation != "COPY" && operation != "OPERATION_TYPE_UNSPECIFIED") {
    throw ApiError::Invalid(
        "The emulator does not support copy operationType " +
        (operation.is_string() ? operation.get<std::string>() : operation.dump()));
  }
  CopyRequest request;
  request.project_id = project_id;
  request.job_id = JobId(body);
  request.copy.destination_table =
      ParseTableReference(copy["destinationTable"], "destination table");
  if (copy.contains("sourceTable") == copy.contains("sourceTables")) {
    throw ApiError::Invalid("Specify sourceTable or sourceTables");
  }
  if (copy.contains("sourceTable")) {
    request.copy.source_tables.push_back(ParseTableReference(copy["sourceTable"], "source table"));
  } else {
    if (!copy["sourceTables"].is_array() || copy["sourceTables"].empty()) {
      throw ApiError::Invalid("sourceTables is required");
    }
    for (const json& table : copy["sourceTables"]) {
      request.copy.source_tables.push_back(ParseTableReference(table, "source table"));
    }
  }
  ParseDispositions(copy, request.copy);
  request.copy.configuration = copy;
  return request;
}

ExtractRequest ParseExtractJob(const std::string& project_id, const json& body) {
  const json config = Configuration(body);
  const json& extract = config["extract"];
  if (!extract.is_object()) throw ApiError::Invalid("Invalid extract configuration");
  if (extract.contains("sourceModel")) {
    throw ApiError::Invalid("The emulator does not support extracting models");
  }
  if (!extract.contains("sourceTable")) throw ApiError::Invalid("Source table is required");
  ExtractRequest request;
  request.project_id = project_id;
  request.job_id = JobId(body);
  request.extract.source_table = ParseTableReference(extract["sourceTable"], "source table");
  // destinationUri is the deprecated spelling of a single destination.
  const json uris = extract.contains("destinationUris")
                        ? extract["destinationUris"]
                        : json::array({extract.value("destinationUri", json())});
  if (!uris.is_array() || uris.empty()) throw ApiError::Invalid("destinationUris is required");
  for (const json& uri : uris) {
    if (!uri.is_string() || uri.get<std::string>().empty()) {
      throw ApiError::Invalid("Invalid destination URI");
    }
    request.extract.destination_uris.push_back(uri.get<std::string>());
  }
  request.extract.configuration = extract;
  return request;
}

}  // namespace

std::string Param(const httplib::Request& request, const char* name) {
  return request.path_params.at(name);
}

bool QueryParamBool(const httplib::Request& request, const char* name) {
  return request.has_param(name) && request.get_param_value(name) == "true";
}

DatasetReference DatasetFromPath(const httplib::Request& request) {
  return DatasetReference{
      .project_id = Param(request, "projectId"),
      .dataset_id = Param(request, "datasetId"),
  };
}

TableReference TableFromPath(const httplib::Request& request) {
  return TableReference{
      .project_id = Param(request, "projectId"),
      .dataset_id = Param(request, "datasetId"),
      .table_id = Param(request, "tableId"),
  };
}

RoutineReference RoutineFromPath(const httplib::Request& request) {
  return RoutineReference{
      .project_id = Param(request, "projectId"),
      .dataset_id = Param(request, "datasetId"),
      .routine_id = Param(request, "routineId"),
  };
}

json ParseBody(const httplib::Request& request) {
  if (request.body.empty()) {
    return json::object();
  }
  try {
    return json::parse(request.body);
  } catch (const json::exception& error) {
    throw ApiError::Invalid(std::string("Invalid JSON body: ") + error.what());
  }
}

bool IsResumableUpload(const httplib::Request& request) {
  return (request.has_param("uploadType") &&
          request.get_param_value("uploadType") == "resumable") ||
         (request.has_param("upload_protocol") &&
          request.get_param_value("upload_protocol") == "resumable");
}

bool IsMultipartUpload(const httplib::Request& request) {
  return request.get_header_value("Content-Type").find("multipart/related") != std::string::npos;
}

MediaUpload ParseMultipartUpload(const httplib::Request& request) {
  const std::string content_type = request.get_header_value("Content-Type");
  const size_t boundary_position = content_type.find("boundary=");
  if (boundary_position == std::string::npos) throw ApiError::Invalid("Missing upload boundary");
  std::string boundary = content_type.substr(boundary_position + 9);
  if (const size_t semicolon = boundary.find(';'); semicolon != std::string::npos) {
    boundary.resize(semicolon);
  }
  if (boundary.size() >= 2 && boundary.front() == '"' && boundary.back() == '"') {
    boundary = boundary.substr(1, boundary.size() - 2);
  }
  const std::string delimiter = "--" + boundary;
  std::vector<std::string> parts;
  size_t start = request.body.find(delimiter);
  while (start != std::string::npos) {
    start += delimiter.size();
    if (request.body.compare(start, 2, "--") == 0) break;
    if (request.body.compare(start, 2, "\r\n") == 0) start += 2;
    const size_t next = request.body.find("\r\n" + delimiter, start);
    if (next == std::string::npos) break;
    const size_t content = request.body.find("\r\n\r\n", start);
    if (content == std::string::npos || content > next) break;
    parts.push_back(request.body.substr(content + 4, next - content - 4));
    start = next + 2;
  }
  if (parts.size() != 2) throw ApiError::Invalid("Invalid multipart upload");
  try {
    return {.metadata = json::parse(parts.at(0)), .content = std::move(parts.at(1))};
  } catch (const json::exception& error) {
    throw ApiError::Invalid(std::string("Invalid upload metadata: ") + error.what());
  }
}

QueryRequest ParseQuery(const std::string& project_id, const json& body) {
  QueryRequest request = ParseQueryConfiguration(project_id, body);
  request.dry_run = body.value("dryRun", false);
  return request;
}

ResultPage ParseQueryPage(const json& body) {
  return {
      .max_results = body.value("maxResults", kDefaultMaxResults),
      .int64_timestamps =
          body.value("formatOptions", json::object()).value("useInt64Timestamp", false),
  };
}

ResultPage ParseResultPage(const httplib::Request& request) {
  return {
      .start_index = request.has_param("pageToken") ? QueryParamInt(request, "pageToken", 0)
                                                    : QueryParamInt(request, "startIndex", 0),
      .max_results = QueryParamInt(request, "maxResults", kDefaultMaxResults),
      .int64_timestamps = QueryParamBool(request, "formatOptions.useInt64Timestamp"),
  };
}

JobRequest ParseJobInsert(const std::string& project_id, const json& body) {
  const json config = Configuration(body);
  if (config.contains("load")) {
    return ParseLoadJob(project_id, body);
  }
  if (config.contains("copy")) {
    return ParseCopyJob(project_id, body);
  }
  if (config.contains("extract")) {
    return ParseExtractJob(project_id, body);
  }
  if (!config.contains("query")) {
    throw ApiError::Invalid("Only query, load, copy, and extract jobs are supported");
  }
  return ParseQueryJob(project_id, body);
}

LoadRequest ParseLoadInsert(const std::string& project_id, const json& body) {
  const json config = Configuration(body);
  if (!config.contains("load")) throw ApiError::Invalid("Media upload requires a load job");
  return ParseLoadJob(project_id, body);
}

JobListRequest ParseJobList(const httplib::Request& request) {
  JobListRequest result;
  // CheckQueryParameters has checked the values of projection and stateFilter.
  result.full_projection = request.get_param_value("projection") != "minimal";
  for (auto [it, end] = request.params.equal_range("stateFilter"); it != end; ++it) {
    result.state_filters.push_back(it->second);
  }
  if (request.has_param("parentJobId")) {
    result.parent_job_id = request.get_param_value("parentJobId");
  }
  result.max_results = QueryParamInt(request, "maxResults", result.max_results);
  result.offset = QueryParamInt(request, "pageToken", result.offset);
  result.min_creation_time = QueryParamInt(request, "minCreationTime", result.min_creation_time);
  result.max_creation_time = QueryParamInt(request, "maxCreationTime", result.max_creation_time);
  if (result.max_results <= 0 || result.offset < 0 || result.min_creation_time < 0 ||
      result.max_creation_time < 0) {
    throw ApiError::Invalid("Invalid jobs.list parameter");
  }
  return result;
}

ListPage ParseListPage(const httplib::Request& request) {
  ListPage result{
      .max_results = QueryParamInt(request, "maxResults", INT64_MAX),
      .page_token = request.get_param_value("pageToken"),
  };
  if (result.max_results <= 0) throw ApiError::Invalid("Invalid value for maxResults");
  return result;
}

DatasetFilter ParseDatasetFilter(const httplib::Request& request) {
  DatasetFilter filters;
  std::istringstream input(request.get_param_value("filter"));
  std::string expression;
  while (input >> expression) {
    if (!expression.starts_with("labels.")) {
      throw ApiError::Invalid("Invalid dataset filter: expected labels.key[:value]");
    }
    const auto colon = expression.find(':');
    const std::string key = expression.substr(7, colon == std::string::npos ? colon : colon - 7);
    if (key.empty() || key.find_first_of(".:*") != std::string::npos ||
        (colon != std::string::npos && expression.find(':', colon + 1) != std::string::npos)) {
      throw ApiError::Invalid("Invalid dataset filter: " + expression);
    }
    std::optional<std::string> value;
    if (colon != std::string::npos && expression.substr(colon + 1) != "*") {
      value = expression.substr(colon + 1);
    }
    if (!filters.emplace(key, std::move(value)).second) {
      throw ApiError::Invalid("Dataset filter label keys must be unique");
    }
    if (filters.size() > 10) {
      throw ApiError::Invalid("Dataset filter supports at most 10 expressions");
    }
  }
  return filters;
}

RoutineListRequest ParseRoutineList(const httplib::Request& request) {
  RoutineListRequest result{.page = ParseListPage(request), .routine_type = {}, .read_mask = {}};
  const std::string filter = request.get_param_value("filter");
  if (!filter.empty()) {
    constexpr std::string_view prefix = "routineType:";
    if (!filter.starts_with(prefix)) {
      throw ApiError::Invalid("Invalid routine filter: expected routineType:{RoutineType}");
    }
    result.routine_type = filter.substr(prefix.size());
    const json& types = Discovery()["schemas"]["Routine"]["properties"]["routineType"]["enum"];
    if (std::ranges::none_of(types,
                             [&](const json& type) { return type == result.routine_type; })) {
      throw ApiError::Invalid("Invalid routine type in filter: " + result.routine_type);
    }
  }
  const std::string mask = request.get_param_value("readMask");
  if (mask == "*") {
    result.read_mask.all = true;
    return result;
  }
  std::string_view remaining = mask.empty() ? "etag,routineReference,routineType,creationTime,"
                                              "lastModifiedTime,language,remoteFunctionOptions"
                                            : std::string_view(mask);
  while (true) {
    const size_t comma = remaining.find(',');
    std::string_view path = remaining.substr(0, comma);
    const json* schema = &Discovery()["schemas"]["Routine"];
    ResourceMask* selection = &result.read_mask;
    while (true) {
      const size_t dot = path.find('.');
      const std::string name(path.substr(0, dot));
      if (!schema->contains("properties") || !(*schema)["properties"].contains(name)) {
        throw ApiError::Invalid("Unknown field in readMask: " + std::string(path));
      }
      selection = &selection->fields[name];
      if (dot == std::string_view::npos) {
        selection->all = true;
        break;
      }
      const json& field = (*schema)["properties"][name];
      // A protobuf field mask permits a repeated field only at the end of a path.
      if (!field.contains("$ref")) {
        throw ApiError::Invalid("Not a message field in readMask: " + name);
      }
      schema = &Discovery()["schemas"][field["$ref"].get<std::string>()];
      path.remove_prefix(dot + 1);
    }
    if (comma == std::string_view::npos) break;
    remaining.remove_prefix(comma + 1);
  }
  return result;
}

TableGetRequest ParseTableGet(const httplib::Request& request) {
  // CheckQueryParameters has checked the value of view.
  return {
      .storage_stats = request.get_param_value("view") != "BASIC",
      .selected_fields = request.get_param_value("selectedFields"),
  };
}

DatasetInsertRequest ParseDatasetInsert(const std::string& project_id, const json& body) {
  if (!body.is_object()) throw ApiError::Invalid("Invalid dataset resource");
  const json reference = body.value("datasetReference", json::object());
  if (!reference.is_object() || !reference.contains("datasetId")) {
    throw ApiError::Invalid("Required parameter is missing: datasetId");
  }
  return {
      .dataset =
          DatasetReference{
              .project_id = project_id,
              .dataset_id = StringField(reference, "datasetId", "dataset ID"),
          },
      .metadata = DatasetMetadataFromJson(body),
  };
}

TableInsertRequest ParseTableInsert(const DatasetReference& dataset, const json& body) {
  const json reference = body.value("tableReference", json::object());
  if (!reference.is_object() || !reference.contains("tableId")) {
    throw ApiError::Invalid("Required parameter is missing: tableId");
  }
  TableInsertRequest request;
  request.table = TableReference{
      .project_id = dataset.project_id,
      .dataset_id = dataset.dataset_id,
      .table_id = StringField(reference, "tableId", "table ID"),
  };
  if (body.contains("view")) {
    request.view = body["view"];
  } else {
    request.schema = SchemaFromJson(body.value("schema", json::object()));
  }
  request.metadata = TableMetadataFromJson(body);
  return request;
}

Routine ParseRoutine(const DatasetReference& dataset, const json& body) {
  if (!body.is_object()) throw ApiError::Invalid("Invalid routine resource");
  const json reference = body.value("routineReference", json::object());
  if (!reference.is_object()) throw ApiError::Invalid("Invalid routine reference");
  for (const auto& [name, value] : reference.items()) {
    if (name != "projectId" && name != "datasetId" && name != "routineId") {
      throw ApiError::Invalid("Invalid routine reference field: " + name);
    }
  }
  const std::string id = StringField(reference, "routineId", "routine ID");
  if (id.empty()) throw ApiError::Invalid("Required parameter is missing: routineId");
  if (StringField(reference, "projectId", "project ID") != dataset.project_id ||
      StringField(reference, "datasetId", "dataset ID") != dataset.dataset_id) {
    throw ApiError::Invalid("Routine reference does not match the request path");
  }
  json resource = json::object();
  for (const auto& [name, value] : body.items()) {
    if (name == "routineReference" || name == "creationTime" || name == "lastModifiedTime" ||
        name == "etag" || name == "buildStatus" || value.is_null()) {
      continue;
    }
    if (name != "routineType" && name != "language" && name != "arguments" &&
        name != "returnType" && name != "definitionBody" && name != "description") {
      throw ApiError::Invalid("The emulator does not support routine field " + name);
    }
    resource[name] = value;
  }
  if (StringField(resource, "routineType", "routine type") != "SCALAR_FUNCTION") {
    throw ApiError::Invalid(
        "The emulator does not support routines other than SQL scalar functions");
  }
  if (resource.contains("language") &&
      StringField(resource, "language", "routine language") != "SQL") {
    throw ApiError::Invalid("The emulator does not support non-SQL routines");
  }
  resource["language"] = "SQL";
  if (StringField(resource, "definitionBody", "routine body").empty()) {
    throw ApiError::Invalid("Required parameter is missing: definitionBody");
  }
  if (resource.contains("description")) StringField(resource, "description", "routine description");
  if (!resource.contains("arguments")) resource["arguments"] = json::array();
  if (!resource["arguments"].is_array()) throw ApiError::Invalid("Invalid routine arguments");
  for (const json& argument : resource["arguments"]) {
    if (!argument.is_object() || StringField(argument, "name", "argument name").empty()) {
      throw ApiError::Invalid("Invalid routine argument");
    }
    for (const auto& [name, value] : argument.items()) {
      if (name != "name" && name != "argumentKind" && name != "dataType") {
        throw ApiError::Invalid("The emulator does not support routine argument field " + name);
      }
    }
    const std::string kind = StringField(argument, "argumentKind", "argument kind");
    if (!kind.empty() && kind != "FIXED_TYPE" && kind != "ANY_TYPE") {
      throw ApiError::Invalid("The emulator does not support routine argument kind " + kind);
    }
    if (kind == "ANY_TYPE" && argument.contains("dataType")) {
      throw ApiError::Invalid("ANY_TYPE arguments cannot have dataType");
    }
  }
  return {
      .reference =
          {
              .project_id = dataset.project_id,
              .dataset_id = dataset.dataset_id,
              .routine_id = id,
          },
      .resource = std::move(resource),
  };
}

TableUpdateRequest ParseTableUpdate(const json& body) {
  if (!body.is_object()) throw ApiError::Invalid("Invalid table resource");
  TableUpdateRequest request;
  if (body.contains("schema") && !body["schema"].is_null()) {
    request.schema = SchemaFromJson(body["schema"]);
  }
  if (body.contains("view") && !body["view"].is_null()) {
    if (!body["view"].is_object()) throw ApiError::Invalid("Invalid view definition");
    request.view = body["view"];
  }
  return request;
}

InsertAllRequest ParseInsertAll(const json& body) {
  if (!body.contains("rows")) {
    throw ApiError::Invalid("Required parameter is missing: rows");
  }
  if (body.contains("templateSuffix") && !body["templateSuffix"].is_null() &&
      (!body["templateSuffix"].is_string() ||
       !body["templateSuffix"].get_ref<const std::string&>().empty())) {
    throw ApiError::Invalid("The emulator does not support templateSuffix");
  }
  return InsertAllRequest{
      .rows = body["rows"],
      .skip_invalid_rows = body.value("skipInvalidRows", false),
      .ignore_unknown_values = body.value("ignoreUnknownValues", false),
  };
}

}  // namespace bigquery_emulator_duckdb::server
