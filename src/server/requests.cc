#include <cstdint>
#include <exception>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "httplib.h"
#include "nlohmann/json.hpp"
#include "src/api_error.h"
#include "src/emulator.h"
#include "src/field_schema.h"
#include "src/query_parameters.h"
#include "src/references.h"
#include "src/server/internal.h"

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
  TableReference result{StringField(table, "projectId", what),
                        StringField(table, "datasetId", what), StringField(table, "tableId", what)};
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
  return DatasetReference{StringField(dataset, "projectId", "default dataset"),
                          StringField(dataset, "datasetId", "default dataset")};
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
  request.load.configuration = load;
  return request;
}

CopyRequest ParseCopyJob(const std::string& project_id, const json& body) {
  const json config = Configuration(body);
  const json& copy = config["copy"];
  if (!copy.is_object() || !copy.contains("destinationTable")) {
    throw ApiError::Invalid("Invalid destination table");
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

}  // namespace

std::string Param(const httplib::Request& request, const char* name) {
  return request.path_params.at(name);
}

bool QueryParamBool(const httplib::Request& request, const char* name) {
  return request.has_param(name) && request.get_param_value(name) == "true";
}

DatasetReference DatasetFromPath(const httplib::Request& request) {
  return DatasetReference{Param(request, "project"), Param(request, "dataset")};
}

TableReference TableFromPath(const httplib::Request& request) {
  return TableReference{Param(request, "project"), Param(request, "dataset"),
                        Param(request, "table")};
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
    return {json::parse(parts[0]), std::move(parts[1])};
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
  return {.max_results = body.value("maxResults", kDefaultMaxResults),
          .int64_timestamps =
              body.value("formatOptions", json::object()).value("useInt64Timestamp", false)};
}

ResultPage ParseResultPage(const httplib::Request& request) {
  return {.start_index = request.has_param("pageToken") ? QueryParamInt(request, "pageToken", 0)
                                                        : QueryParamInt(request, "startIndex", 0),
          .max_results = QueryParamInt(request, "maxResults", kDefaultMaxResults),
          .int64_timestamps = QueryParamBool(request, "formatOptions.useInt64Timestamp")};
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
    throw ApiError::Invalid("The emulator does not support extract jobs");
  }
  if (!config.contains("query")) {
    throw ApiError::Invalid("Only query, load, and copy jobs are supported");
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
  const std::string projection =
      request.has_param("projection") ? request.get_param_value("projection") : "full";
  if (projection != "full" && projection != "minimal") {
    throw ApiError::Invalid("Invalid value for projection");
  }
  result.full_projection = projection == "full";
  result.state_filter =
      request.has_param("stateFilter") ? request.get_param_value("stateFilter") : "";
  if (!result.state_filter.empty() && result.state_filter != "done" &&
      result.state_filter != "pending" && result.state_filter != "running") {
    throw ApiError::Invalid("Invalid value for stateFilter");
  }
  result.parent_filter = request.has_param("parentJobId");
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
  ListPage result{.max_results = QueryParamInt(request, "maxResults", INT64_MAX),
                  .page_token = request.get_param_value("pageToken")};
  if (result.max_results <= 0) throw ApiError::Invalid("Invalid value for maxResults");
  return result;
}

DatasetReference ParseDatasetInsert(const std::string& project_id, const json& body) {
  const json reference = body.value("datasetReference", json::object());
  if (!reference.is_object() || !reference.contains("datasetId")) {
    throw ApiError::Invalid("Required parameter is missing: datasetId");
  }
  return DatasetReference{project_id, StringField(reference, "datasetId", "dataset ID")};
}

TableInsertRequest ParseTableInsert(const DatasetReference& dataset, const json& body) {
  const json reference = body.value("tableReference", json::object());
  if (!reference.is_object() || !reference.contains("tableId")) {
    throw ApiError::Invalid("Required parameter is missing: tableId");
  }
  TableInsertRequest request;
  request.table = TableReference{dataset.project_id, dataset.dataset_id,
                                 StringField(reference, "tableId", "table ID")};
  if (body.contains("view")) {
    request.view = body["view"];
  } else {
    request.schema = SchemaFromJson(body.value("schema", json::object()));
  }
  return request;
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
      body["templateSuffix"] != "") {
    throw ApiError::Invalid("The emulator does not support templateSuffix");
  }
  return InsertAllRequest{.rows = body["rows"],
                          .skip_invalid_rows = body.value("skipInvalidRows", false),
                          .ignore_unknown_values = body.value("ignoreUnknownValues", false)};
}

}  // namespace bigquery_emulator_duckdb::server
