#include "src/server/requests.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/log/absl_log.h"
#include "absl/time/time.h"
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

// The routine fields ParseRoutine handles, although it supports only some of their values.
const ResourceMask& RoutineFields() {
  static const ResourceMask* const kFields =
      new ResourceMask(MaskOf("Routine", {
                                             "routineReference",
                                             "routineType",
                                             "language",
                                             "arguments.name",
                                             "arguments.argumentKind",
                                             "arguments.dataType",
                                             "arguments.mode",
                                             "returnType",
                                             "returnTableType",
                                             "definitionBody",
                                             "description",
                                             "strictMode",
                                             "pythonOptions.entryPoint",
                                             "externalRuntimeOptions.runtimeVersion",
                                         }));
  return *kFields;
}

// The fields of a Dataset resource datasets.insert, patch and update handle, although they
// support only some of their values.
const ResourceMask& DatasetFields() {
  static const ResourceMask* const kFields =
      new ResourceMask(MaskOf("Dataset", {
                                             "datasetReference",
                                             "description",
                                             "friendlyName",
                                             "labels",
                                             "resourceTags",
                                             "defaultTableExpirationMs",
                                             "defaultRoundingMode",
                                             "location",
                                         }));
  return *kFields;
}

// The fields of a Table resource tables.insert, patch and update handle, although they support
// only some of their values.
const ResourceMask& TableFields() {
  static const ResourceMask* const kFields =
      new ResourceMask(MaskOf("Table", {
                                           "tableReference",
                                           "schema.fields.name",
                                           "schema.fields.type",
                                           "schema.fields.mode",
                                           "schema.fields.fields",
                                           "schema.fields.description",
                                           "schema.fields.maxLength",
                                           "schema.fields.precision",
                                           "schema.fields.scale",
                                           "schema.fields.roundingMode",
                                           "schema.fields.rangeElementType",
                                           "schema.fields.defaultValueExpression",
                                           "schema.fields.policyTags",
                                           "view.query",
                                           "view.useLegacySql",
                                           "materializedView.query",
                                           "materializedView.enableRefresh",
                                           "materializedView.allowNonIncrementalDefinition",
                                           "timePartitioning",
                                           "rangePartitioning",
                                           "clustering",
                                           "requirePartitionFilter",
                                           "description",
                                           "friendlyName",
                                           "labels",
                                           "resourceTags",
                                           "expirationTime",
                                           "defaultRoundingMode",
                                           "tableConstraints",
                                           "encryptionConfiguration",
                                       }));
  return *kFields;
}

// The fields of a job resource ParseJobInsert handles, although it supports only some of their
// values.
const ResourceMask& JobFields() {
  static const ResourceMask* const kFields = new ResourceMask(MaskOf(
      "Job", {
                 "jobReference",
                 "configuration.dryRun",
                 "configuration.query.query",
                 "configuration.query.useLegacySql",
                 "configuration.query.defaultDataset",
                 "configuration.query.queryParameters",
                 "configuration.query.parameterMode",
                 "configuration.query.destinationTable",
                 "configuration.query.createDisposition",
                 "configuration.query.writeDisposition",
                 "configuration.query.destinationEncryptionConfiguration",
                 "configuration.load.destinationTable",
                 "configuration.load.createDisposition",
                 "configuration.load.writeDisposition",
                 "configuration.load.destinationEncryptionConfiguration",
                 "configuration.load.sourceUris",
                 "configuration.load.sourceFormat",
                 "configuration.load.schema",
                 "configuration.load.skipLeadingRows",
                 "configuration.load.fieldDelimiter",
                 "configuration.load.parquetOptions.enableListInference",
                 "configuration.load.decimalTargetTypes",
                 // Loads detect a schema whenever neither they nor their destination give one.
                 "configuration.load.autodetect",
                 "configuration.copy",
                 "configuration.extract.sourceTable",
                 "configuration.extract.destinationUri",
                 "configuration.extract.destinationUris",
                 "configuration.extract.destinationFormat",
                 "configuration.extract.compression",
                 "configuration.extract.fieldDelimiter",
                 "configuration.extract.printHeader",
             }));
  return *kFields;
}

// The fields of a jobs.query request ParseQuery handles, although it supports only some of their
// values.
const ResourceMask& QueryRequestFields() {
  static const ResourceMask* const kFields = new ResourceMask(
      MaskOf("QueryRequest",
             {
                 "kind",
                 "query",
                 "useLegacySql",
                 "defaultDataset",
                 "queryParameters",
                 "parameterMode",
                 "destinationEncryptionConfiguration",
                 "location",
                 "dryRun",
                 "maxResults",
                 "formatOptions.useInt64Timestamp",
                 // Ignored: jobs finish before they are inserted, the emulator keeps no job labels
                 // and caches no results, and clients such as the Go client send a requestId with
                 // every request for BigQuery to deduplicate their retries.
                 "timeoutMs",
                 "labels",
                 "useQueryCache",
                 "requestId",
             }));
  return *kFields;
}

constexpr auto kProcedureArgumentModes = std::to_array<std::string_view>({
    "IN",
    "OUT",
    "INOUT",
});

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
  if (!object.contains(key)) {
    return "";
  }
  if (!object[key].is_string()) {
    throw ApiError::Invalid("Invalid " + std::string(what));
  }
  return object[key].get<std::string>();
}

// Reads a TableReference, whose project defaults to the job's. `what` names it in errors.
TableReference ParseTableReference(const json& table, std::string_view what) {
  TableReference result{
      .project_id = table.value("projectId", ""),
      .dataset_id = table.value("datasetId", ""),
      .table_id = table.value("tableId", ""),
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
  return DatasetReference{
      .project_id = config["defaultDataset"].value("projectId", ""),
      .dataset_id = config["defaultDataset"].value("datasetId", ""),
  };
}

// jobs.query takes the query configuration as the request body and jobs.insert takes it as
// configuration.query, but the fields this emulator reads are spelled the same in both.
QueryRequest ParseQueryConfiguration(const std::string& project_id, const json& config) {
  if (!config.contains("query")) {
    throw ApiError::Invalid("Required parameter is missing: query");
  }
  // Only GoogleSQL is emulated. An omitted useLegacySql runs as GoogleSQL, although BigQuery
  // defaults it to true.
  if (config.value("useLegacySql", false)) {
    throw ApiError::Unsupported("legacy SQL; set useLegacySql to false");
  }
  RejectEncryptionConfiguration(config, "destinationEncryptionConfiguration",
                                "destinationEncryptionConfiguration");
  QueryRequest request;
  request.project_id = project_id;
  request.query = config["query"].get<std::string>();
  request.default_dataset = ParseDefaultDataset(config);
  const json parameters = config.value("queryParameters", json::array());
  // Logged as the request spells them, before parsing, so that parameters that fail to parse are
  // logged too.
  if (!parameters.empty()) {
    ABSL_VLOG(1) << "Query parameters: " << parameters.dump();
  }
  request.parameters = QueryParameters::Parse(parameters);
  return request;
}

// Reads the dispositions in a job configuration into `job`, which keeps its job type's default
// for any the configuration omits.
template <typename T>
void ParseDispositions(const json& config, T& job) {
  if (config.contains("createDisposition")) {
    const std::string name = config["createDisposition"].get<std::string>();
    const std::optional<CreateDisposition> disposition = ParseCreateDisposition(name);
    if (!disposition.has_value()) {
      throw ApiError::Invalid("Invalid create disposition: " + name);
    }
    job.create_disposition = *disposition;
  }
  if (config.contains("writeDisposition")) {
    const std::string name = config["writeDisposition"].get<std::string>();
    const std::optional<WriteDisposition> disposition = ParseWriteDisposition(name);
    if (!disposition.has_value()) {
      throw ApiError::Invalid("Invalid write disposition: " + name);
    }
    job.write_disposition = *disposition;
  }
}

std::string JobId(const json& job) {
  return job.value(json::json_pointer("/jobReference/jobId"), "");
}

// The location `object`, such as a job reference, names, or nothing when it names none.
std::optional<std::string> LocationField(const json& object) {
  std::string location = object.value("location", "");
  if (location.empty()) {
    return std::nullopt;
  }
  return location;
}

// The location the job reference of a job resource names.
std::optional<std::string> JobLocationField(const json& job) {
  return LocationField(job.value("jobReference", json::object()));
}

QueryRequest ParseQueryJob(const std::string& project_id, const json& job) {
  const json& config = job["configuration"];
  const json& query = config["query"];
  QueryRequest request = ParseQueryConfiguration(project_id, query);
  request.job_id = JobId(job);
  request.location = JobLocationField(job);
  request.dry_run = config.value("dryRun", false);
  if (query.contains("destinationTable")) {
    request.destination_table = ParseTableReference(query["destinationTable"], "destination table");
  }
  ParseDispositions(query, request);
  return request;
}

LoadRequest ParseLoadJob(const std::string& project_id, const json& job) {
  const json& load = job["configuration"]["load"];
  if (!load.contains("destinationTable")) {
    throw ApiError::Invalid("Invalid destination table");
  }
  LoadRequest request;
  request.project_id = project_id;
  request.job_id = JobId(job);
  request.location = JobLocationField(job);
  request.load.destination_table =
      ParseTableReference(load["destinationTable"], "destination table");
  ParseDispositions(load, request.load);
  RejectEncryptionConfiguration(load, "destinationEncryptionConfiguration",
                                "destinationEncryptionConfiguration");
  request.load.configuration = load;
  return request;
}

CopyRequest ParseCopyJob(const std::string& project_id, const json& job) {
  const json& copy = job["configuration"]["copy"];
  if (!copy.contains("destinationTable")) {
    throw ApiError::Invalid("Invalid destination table");
  }
  CopyRequest request;
  if (const std::string operation = copy.value("operationType", ""); operation == "SNAPSHOT") {
    request.copy.operation = CopyOperation::kSnapshot;
  } else if (operation == "RESTORE") {
    request.copy.operation = CopyOperation::kRestore;
  } else if (operation == "CLONE") {
    request.copy.operation = CopyOperation::kClone;
  }
  request.project_id = project_id;
  request.job_id = JobId(job);
  request.location = JobLocationField(job);
  request.copy.destination_table =
      ParseTableReference(copy["destinationTable"], "destination table");
  if (copy.contains("sourceTable") == copy.contains("sourceTables")) {
    throw ApiError::Invalid("Specify sourceTable or sourceTables");
  }
  if (copy.contains("sourceTable")) {
    request.copy.source_tables.push_back(ParseTableReference(copy["sourceTable"], "source table"));
  } else {
    if (copy["sourceTables"].empty()) {
      throw ApiError::Invalid("sourceTables is required");
    }
    for (const json& table : copy["sourceTables"]) {
      request.copy.source_tables.push_back(ParseTableReference(table, "source table"));
    }
  }
  ParseDispositions(copy, request.copy);
  RejectEncryptionConfiguration(copy, "destinationEncryptionConfiguration",
                                "destinationEncryptionConfiguration");
  // An RFC 3339 timestamp, as Timestamp is in JSON.
  if (copy.contains("destinationExpirationTime")) {
    const std::string expiration = copy["destinationExpirationTime"].get<std::string>();
    absl::Time time;
    std::string error;
    if (!absl::ParseTime(absl::RFC3339_full, expiration, &time, &error)) {
      throw ApiError::Invalid("Invalid destinationExpirationTime: " +
                              copy["destinationExpirationTime"].dump());
    }
    request.copy.destination_expiration_time = absl::ToUnixMillis(time);
  }
  request.copy.configuration = copy;
  return request;
}

ExtractRequest ParseExtractJob(const std::string& project_id, const json& job) {
  const json& extract = job["configuration"]["extract"];
  if (!extract.contains("sourceTable")) {
    throw ApiError::Invalid("Source table is required");
  }
  ExtractRequest request;
  request.project_id = project_id;
  request.job_id = JobId(job);
  request.location = JobLocationField(job);
  request.extract.source_table = ParseTableReference(extract["sourceTable"], "source table");
  // destinationUri is the deprecated spelling of a single destination.
  const json uris = extract.contains("destinationUris")
                        ? extract["destinationUris"]
                        : json::array({extract.value("destinationUri", "")});
  if (uris.empty()) {
    throw ApiError::Invalid("destinationUris is required");
  }
  for (const json& uri : uris) {
    if (uri.get<std::string>().empty()) {
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

std::optional<std::string> LocationParam(const httplib::Request& request) {
  if (!request.has_param("location") || request.get_param_value("location").empty()) {
    return std::nullopt;
  }
  return request.get_param_value("location");
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
  if (boundary_position == std::string::npos) {
    throw ApiError::Invalid("Missing upload boundary");
  }
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
    if (request.body.compare(start, 2, "--") == 0) {
      break;
    }
    if (request.body.compare(start, 2, "\r\n") == 0) {
      start += 2;
    }
    const size_t next = request.body.find("\r\n" + delimiter, start);
    if (next == std::string::npos) {
      break;
    }
    const size_t content = request.body.find("\r\n\r\n", start);
    if (content == std::string::npos || content > next) {
      break;
    }
    parts.push_back(request.body.substr(content + 4, next - content - 4));
    start = next + 2;
  }
  if (parts.size() != 2) {
    throw ApiError::Invalid("Invalid multipart upload");
  }
  try {
    return {.metadata = json::parse(parts.at(0)), .content = std::move(parts.at(1))};
  } catch (const json::exception& error) {
    throw ApiError::Invalid(std::string("Invalid upload metadata: ") + error.what());
  }
}

JobsQuery ParseQuery(const std::string& project_id, const json& body) {
  const json checked =
      CheckResource(body, "QueryRequest", QueryRequestFields(), UnknownFields::kIgnore);
  JobsQuery result{
      .query = ParseQueryConfiguration(project_id, checked),
      .page =
          {
              .max_results = checked.value("maxResults", kDefaultMaxResults),
              .int64_timestamps =
                  checked.value(json::json_pointer("/formatOptions/useInt64Timestamp"), false),
          },
  };
  result.query.location = LocationField(checked);
  result.query.dry_run = checked.value("dryRun", false);
  return result;
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
  const json job = CheckResource(body, "Job", JobFields(), UnknownFields::kIgnore);
  const json config = job.value("configuration", json::object());
  if (config.contains("load")) {
    return ParseLoadJob(project_id, job);
  }
  if (config.contains("copy")) {
    return ParseCopyJob(project_id, job);
  }
  if (config.contains("extract")) {
    return ParseExtractJob(project_id, job);
  }
  if (!config.contains("query")) {
    throw ApiError::Invalid("Only query, load, copy, and extract jobs are supported");
  }
  return ParseQueryJob(project_id, job);
}

LoadRequest ParseLoadInsert(const std::string& project_id, const json& body) {
  const json job = CheckResource(body, "Job", JobFields(), UnknownFields::kIgnore);
  if (!job.contains("configuration") || !job["configuration"].contains("load")) {
    throw ApiError::Invalid("Media upload requires a load job");
  }
  return ParseLoadJob(project_id, job);
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
  if (result.max_results <= 0) {
    throw ApiError::Invalid("Invalid value for maxResults");
  }
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
    if (comma == std::string_view::npos) {
      break;
    }
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

// BigQuery ignores the fields a Dataset or Table resource lacks, at any depth, as it does a job's.
DatasetInsertRequest ParseDatasetInsert(const std::string& project_id, const json& resource) {
  const json body = CheckResource(resource, "Dataset", DatasetFields(), UnknownFields::kIgnore);
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

TableInsertRequest ParseTableInsert(const DatasetReference& dataset, const json& resource) {
  const json body = CheckResource(resource, "Table", TableFields(), UnknownFields::kIgnore);
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
  if (body.contains("materializedView")) {
    if (body.contains("view")) {
      throw ApiError::Invalid("Cannot specify both view and materializedView");
    }
    request.materialized = true;
    request.view = body["materializedView"];
  } else if (body.contains("view")) {
    if (!body.value("tableConstraints", json()).is_null()) {
      throw ApiError::Invalid("Cannot specify the table constraints when creating view.");
    }
    request.view = body["view"];
  } else {
    request.schema = SchemaFromJson(body.value("schema", json::object()));
  }
  request.metadata = TableMetadataFromJson(body);
  return request;
}

Routine ParseRoutine(const DatasetReference& dataset, const json& body) {
  json resource = CheckResource(body, "Routine", RoutineFields());
  const json reference = resource.value("routineReference", json::object());
  resource.erase("routineReference");
  const std::string id = reference.value("routineId", "");
  if (id.empty()) {
    throw ApiError::Invalid("Required parameter is missing: routineId");
  }
  if (reference.value("projectId", "") != dataset.project_id ||
      reference.value("datasetId", "") != dataset.dataset_id) {
    throw ApiError::Invalid("Routine reference does not match the request path");
  }
  const std::string type = resource.value("routineType", "");
  const bool procedure = type == "PROCEDURE";
  const bool tvf = type == "TABLE_VALUED_FUNCTION";
  if (!procedure && !tvf && type != "SCALAR_FUNCTION") {
    throw ApiError::Unsupported(
        "routines other than SQL scalar functions, table functions and procedures");
  }
  if (!procedure && resource.contains("strictMode")) {
    throw ApiError::Unsupported("strictMode on functions");
  }
  if (resource.contains("returnType") && (procedure || tvf)) {
    throw ApiError::Invalid("Only scalar functions can have returnType");
  }
  if (resource.contains("returnTableType") && !tvf) {
    throw ApiError::Invalid("Only table functions can have returnTableType");
  }
  const std::string language = resource.value("language", "SQL");
  if (language != "SQL" && language != "PYTHON") {
    throw ApiError::Unsupported("non-SQL routines other than Python scalar functions");
  }
  resource["language"] = language;
  if (language == "PYTHON") {
    if (procedure || tvf || !resource.contains("returnType") ||
        resource.value(json::json_pointer("/externalRuntimeOptions/runtimeVersion"), "") !=
            "python-3.11") {
      throw ApiError::Unsupported("Python routines without a fixed result and python-3.11 runtime");
    }
    if (resource.value(json::json_pointer("/pythonOptions/entryPoint"), "").empty()) {
      throw ApiError::Unsupported("Python routines without an entryPoint");
    }
  } else if (resource.contains("externalRuntimeOptions") || resource.contains("pythonOptions")) {
    throw ApiError::Unsupported("Python options on SQL routines");
  }
  if (resource.value("definitionBody", "").empty()) {
    throw ApiError::Invalid("Required parameter is missing: definitionBody");
  }
  if (!resource.contains("arguments")) {
    resource["arguments"] = json::array();
  }
  for (const json& argument : resource["arguments"]) {
    if (argument.value("name", "").empty()) {
      throw ApiError::Invalid("Invalid routine argument");
    }
    if (argument.contains("mode")) {
      const std::string mode = argument["mode"].get<std::string>();
      if (!procedure) {
        throw ApiError::Unsupported("routine argument field mode");
      }
      if (std::ranges::find(kProcedureArgumentModes, mode) == kProcedureArgumentModes.end()) {
        throw ApiError::Unsupported("procedure argument mode " + mode);
      }
    }
    const std::string kind = argument.value("argumentKind", "");
    if ((procedure || tvf) && kind == "ANY_TYPE") {
      throw ApiError::Unsupported("procedure or TVF arguments of ANY TYPE");
    }
    if (!kind.empty() && kind != "FIXED_TYPE" && kind != "ANY_TYPE") {
      throw ApiError::Unsupported("routine argument kind " + kind);
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

json ParseDatasetUpdate(const json& body, bool patch) {
  return CheckResource(body, "Dataset", DatasetFields(), UnknownFields::kIgnore,
                       patch ? Change::kPatch : Change::kUpdate);
}

TableUpdateRequest ParseTableUpdate(const json& resource, bool patch) {
  const json body = CheckResource(resource, "Table", TableFields(), UnknownFields::kIgnore,
                                  patch ? Change::kPatch : Change::kUpdate);
  if (!body.value("materializedView", json()).is_null()) {
    throw ApiError::Unsupported("updating materializedView");
  }
  TableUpdateRequest request{.resource = body, .schema = std::nullopt, .view = std::nullopt};
  // As BigQuery answers, a patch that sets the schema to null keeps it; the emulator keeps a view
  // set to null too.
  if (!body.value("schema", json()).is_null()) {
    request.schema = SchemaFromJson(body["schema"]);
  }
  if (!body.value("view", json()).is_null()) {
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
    throw ApiError::Unsupported("templateSuffix");
  }
  return InsertAllRequest{
      .rows = body["rows"],
      .skip_invalid_rows = body.value("skipInvalidRows", false),
      .ignore_unknown_values = body.value("ignoreUnknownValues", false),
  };
}

}  // namespace bigquery_emulator_duckdb::server
