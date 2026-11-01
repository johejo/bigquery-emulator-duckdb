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

#include "absl/log/absl_log.h"
#include "absl/status/status.h"
#include "google/cloud/bigquery/v2/dataset.pb.h"
#include "google/cloud/bigquery/v2/job.pb.h"
#include "google/cloud/bigquery/v2/job_config.pb.h"
#include "google/cloud/bigquery/v2/routine.pb.h"
#include "google/cloud/bigquery/v2/routine_reference.pb.h"
#include "google/cloud/bigquery/v2/table.pb.h"
#include "google/cloud/bigquery/v2/table_reference.pb.h"
#include "google/protobuf/timestamp.pb.h"
#include "google/protobuf/util/json_util.h"
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
#include "src/timestamp_precision.h"

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
                                             "defaultCollation",
                                             "location",
                                         }));
  return *kFields;
}

// The fields of a Table resource tables.insert, patch and update handle, although they support
// only some of their values.
const ResourceMask& TableFields() {
  static const ResourceMask* const kFields = new ResourceMask(
      MaskOf("Table", {
                          "tableReference",
                          "externalDataConfiguration.sourceFormat",
                          "externalDataConfiguration.sourceUris",
                          "externalDataConfiguration.autodetect",
                          "externalDataConfiguration.parquetOptions.enableListInference",
                          "externalDataConfiguration.csvOptions.skipLeadingRows",
                          "externalDataConfiguration.csvOptions.fieldDelimiter",
                          "externalDataConfiguration.csvOptions.quote",
                          "externalDataConfiguration.csvOptions.allowQuotedNewlines",
                          "externalDataConfiguration.csvOptions.allowJaggedRows",
                          "externalDataConfiguration.csvOptions.nullMarker",
                          "externalDataConfiguration.csvOptions.nullMarkers",
                          "externalDataConfiguration.csvOptions.preserveAsciiControlCharacters",
                          "externalDataConfiguration.ignoreUnknownValues",
                          "externalDataConfiguration.schema.fields.name",
                          "externalDataConfiguration.schema.fields.type",
                          "externalDataConfiguration.schema.fields.mode",
                          "externalDataConfiguration.schema.fields.fields",
                          "externalDataConfiguration.schema.fields.description",
                          "externalDataConfiguration.schema.fields.maxLength",
                          "externalDataConfiguration.schema.fields.precision",
                          "externalDataConfiguration.schema.fields.scale",
                          "externalDataConfiguration.schema.fields.roundingMode",
                          "externalDataConfiguration.schema.fields.rangeElementType",
                          "externalDataConfiguration.schema.fields.defaultValueExpression",
                          "externalDataConfiguration.schema.fields.policyTags",
                          "externalDataConfiguration.schema.fields.collation",
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
                          "schema.fields.collation",
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
                          "defaultCollation",
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
                 "configuration.load.quote",
                 "configuration.load.allowQuotedNewlines",
                 "configuration.load.allowJaggedRows",
                 "configuration.load.ignoreUnknownValues",
                 "configuration.load.nullMarker",
                 "configuration.load.nullMarkers",
                 "configuration.load.preserveAsciiControlCharacters",
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

using nlohmann::json;
namespace v2 = google::cloud::bigquery::v2;

// A resource CheckResource has checked, as the protocol buffer message of its schema. Values that
// it leaves unchecked, such as malformed timestamps, are rejected. Fields the message lacks, such
// as JobConfigurationExtract's deprecated destinationUri, are left out; read them from the JSON.
template <typename Message>
Message ToProto(const json& resource) {
  Message message;
  google::protobuf::util::JsonParseOptions options;
  options.ignore_unknown_fields = true;
  if (const absl::Status status =
          google::protobuf::util::JsonStringToMessage(resource.dump(), &message, options);
      !status.ok()) {
    throw ApiError::Invalid("Invalid value: " + std::string(status.message()));
  }
  return message;
}

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

// Reads a TableReference, whose project defaults to the job's. `what` names it in errors.
TableReference ParseTableReference(const v2::TableReference& table, std::string_view what) {
  if (table.dataset_id().empty() || table.table_id().empty()) {
    throw ApiError::Invalid("Invalid " + std::string(what));
  }
  return {
      .project_id = table.project_id(),
      .dataset_id = table.dataset_id(),
      .table_id = table.table_id(),
  };
}

// jobs.query takes the query configuration as a QueryRequest and jobs.insert takes it as a
// JobConfigurationQuery, but the fields this emulator reads are spelled the same in both.
// `config` is the configuration's JSON, for the query parameters.
template <typename Message>
QueryRequest ParseQueryConfiguration(const std::string& project_id, const Message& message,
                                     const json& config) {
  if (!config.contains("query")) {
    throw ApiError::Invalid("Required parameter is missing: query");
  }
  // Only GoogleSQL is emulated. An omitted useLegacySql runs as GoogleSQL, although BigQuery
  // defaults it to true.
  if (message.use_legacy_sql().value()) {
    throw ApiError::Unsupported("legacy SQL; set useLegacySql to false");
  }
  RejectEncryptionConfiguration(config, "destinationEncryptionConfiguration",
                                "destinationEncryptionConfiguration");
  QueryRequest request;
  request.project_id = project_id;
  request.query = message.query();
  if (message.has_default_dataset()) {
    request.default_dataset = DatasetReference{
        .project_id = message.default_dataset().project_id(),
        .dataset_id = message.default_dataset().dataset_id(),
    };
  }
  const json parameters = config.value("queryParameters", json::array());
  // Logged as the request spells them, before parsing, so that parameters that fail to parse are
  // logged too.
  if (!parameters.empty()) {
    ABSL_VLOG(1) << "Query parameters: " << parameters.dump();
  }
  // The server runs only BigQuery's semantics.
  request.parameters = QueryParameters::Parse(parameters, TimestampPrecision::kMicroseconds);
  return request;
}

// Reads the dispositions of a job configuration into `job`, which keeps its job type's default
// for any the configuration leaves empty, as BigQuery cannot tell an empty one from one left out.
template <typename T>
void ParseDispositions(const std::string& create, const std::string& write, T& job) {
  if (!create.empty()) {
    const std::optional<CreateDisposition> disposition = ParseCreateDisposition(create);
    if (!disposition.has_value()) {
      throw ApiError::Invalid("Invalid create disposition: " + create);
    }
    job.create_disposition = *disposition;
  }
  if (!write.empty()) {
    const std::optional<WriteDisposition> disposition = ParseWriteDisposition(write);
    if (!disposition.has_value()) {
      throw ApiError::Invalid("Invalid write disposition: " + write);
    }
    job.write_disposition = *disposition;
  }
}

// A location, or nothing when it is empty.
std::optional<std::string> Location(const std::string& location) {
  if (location.empty()) {
    return std::nullopt;
  }
  return location;
}

// Each job type's parser takes the checked job resource as its message and the JSON of its
// configuration, which its request keeps.
QueryRequest ParseQueryJob(const std::string& project_id, const v2::Job& job,
                           const json& configuration) {
  const v2::JobConfigurationQuery& query = job.configuration().query();
  QueryRequest request = ParseQueryConfiguration(project_id, query, configuration);
  request.job_id = job.job_reference().job_id();
  request.location = Location(job.job_reference().location().value());
  request.dry_run = job.configuration().dry_run().value();
  if (query.has_destination_table()) {
    request.destination_table = ParseTableReference(query.destination_table(), "destination table");
  }
  ParseDispositions(query.create_disposition(), query.write_disposition(), request);
  return request;
}

LoadRequest ParseLoadJob(const std::string& project_id, const v2::Job& job,
                         const json& configuration) {
  const v2::JobConfigurationLoad& load = job.configuration().load();
  if (!load.has_destination_table()) {
    throw ApiError::Invalid("Invalid destination table");
  }
  LoadRequest request;
  request.project_id = project_id;
  request.job_id = job.job_reference().job_id();
  request.location = Location(job.job_reference().location().value());
  request.load.destination_table =
      ParseTableReference(load.destination_table(), "destination table");
  ParseDispositions(load.create_disposition(), load.write_disposition(), request.load);
  RejectEncryptionConfiguration(configuration, "destinationEncryptionConfiguration",
                                "destinationEncryptionConfiguration");
  request.load.configuration = configuration;
  return request;
}

CopyRequest ParseCopyJob(const std::string& project_id, const v2::Job& job,
                         const json& configuration) {
  const v2::JobConfigurationTableCopy& copy = job.configuration().copy();
  if (!copy.has_destination_table()) {
    throw ApiError::Invalid("Invalid destination table");
  }
  CopyRequest request;
  switch (copy.operation_type()) {
    case v2::JobConfigurationTableCopy::SNAPSHOT:
      request.copy.operation = CopyOperation::kSnapshot;
      break;
    case v2::JobConfigurationTableCopy::RESTORE:
      request.copy.operation = CopyOperation::kRestore;
      break;
    case v2::JobConfigurationTableCopy::CLONE:
      request.copy.operation = CopyOperation::kClone;
      break;
    default:
      break;
  }
  request.project_id = project_id;
  request.job_id = job.job_reference().job_id();
  request.location = Location(job.job_reference().location().value());
  request.copy.destination_table =
      ParseTableReference(copy.destination_table(), "destination table");
  if (copy.has_source_table() == !copy.source_tables().empty()) {
    throw ApiError::Invalid("Specify sourceTable or sourceTables");
  }
  if (copy.has_source_table()) {
    request.copy.source_tables.push_back(ParseTableReference(copy.source_table(), "source table"));
  }
  for (const v2::TableReference& table : copy.source_tables()) {
    request.copy.source_tables.push_back(ParseTableReference(table, "source table"));
  }
  ParseDispositions(copy.create_disposition(), copy.write_disposition(), request.copy);
  RejectEncryptionConfiguration(configuration, "destinationEncryptionConfiguration",
                                "destinationEncryptionConfiguration");
  if (copy.has_destination_expiration_time()) {
    // Timestamps keep their nanoseconds nonnegative, so this rounds down as milliseconds do.
    const google::protobuf::Timestamp& expiration = copy.destination_expiration_time();
    request.copy.destination_expiration_time =
        (expiration.seconds() * 1000) + (expiration.nanos() / 1000000);
  }
  request.copy.configuration = configuration;
  return request;
}

ExtractRequest ParseExtractJob(const std::string& project_id, const v2::Job& job,
                               const json& configuration) {
  const v2::JobConfigurationExtract& extract = job.configuration().extract();
  if (!extract.has_source_table()) {
    throw ApiError::Invalid("Source table is required");
  }
  ExtractRequest request;
  request.project_id = project_id;
  request.job_id = job.job_reference().job_id();
  request.location = Location(job.job_reference().location().value());
  request.extract.source_table = ParseTableReference(extract.source_table(), "source table");
  request.extract.destination_uris.assign(extract.destination_uris().begin(),
                                          extract.destination_uris().end());
  // destinationUri, the deprecated spelling of a single destination, is missing from the
  // protocol buffer.
  if (request.extract.destination_uris.empty()) {
    request.extract.destination_uris.push_back(configuration.value("destinationUri", ""));
  }
  if (std::ranges::find(request.extract.destination_uris, "") !=
      request.extract.destination_uris.end()) {
    throw ApiError::Invalid("Invalid destination URI");
  }
  request.extract.configuration = configuration;
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
  const auto message = ToProto<v2::QueryRequest>(checked);
  JobsQuery result{
      .query = ParseQueryConfiguration(project_id, message, checked),
      .page =
          {
              .max_results =
                  message.has_max_results() ? message.max_results().value() : kDefaultMaxResults,
              .int64_timestamps = message.format_options().use_int64_timestamp(),
          },
  };
  result.query.location = Location(message.location());
  result.query.dry_run = message.dry_run();
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
  const auto message = ToProto<v2::Job>(job);
  const v2::JobConfiguration& config = message.configuration();
  if (config.has_load()) {
    return ParseLoadJob(project_id, message, job["configuration"]["load"]);
  }
  if (config.has_copy()) {
    return ParseCopyJob(project_id, message, job["configuration"]["copy"]);
  }
  if (config.has_extract()) {
    return ParseExtractJob(project_id, message, job["configuration"]["extract"]);
  }
  if (!config.has_query()) {
    throw ApiError::Invalid("Only query, load, copy, and extract jobs are supported");
  }
  return ParseQueryJob(project_id, message, job["configuration"]["query"]);
}

LoadRequest ParseLoadInsert(const std::string& project_id, const json& body) {
  const json job = CheckResource(body, "Job", JobFields(), UnknownFields::kIgnore);
  const auto message = ToProto<v2::Job>(job);
  if (!message.configuration().has_load()) {
    throw ApiError::Invalid("Media upload requires a load job");
  }
  return ParseLoadJob(project_id, message, job["configuration"]["load"]);
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
  const auto dataset = ToProto<v2::Dataset>(body);
  if (dataset.dataset_reference().dataset_id().empty()) {
    throw ApiError::Required("dataset_id");
  }
  return {
      .dataset =
          DatasetReference{
              .project_id = project_id,
              .dataset_id = dataset.dataset_reference().dataset_id(),
          },
      .metadata = DatasetMetadataFromJson(body),
  };
}

TableInsertRequest ParseTableInsert(const DatasetReference& dataset, const json& resource) {
  const json body = CheckResource(resource, "Table", TableFields(), UnknownFields::kIgnore);
  const auto table = ToProto<v2::Table>(body);
  if (table.table_reference().table_id().empty()) {
    throw ApiError::Required("table_id");
  }
  TableInsertRequest request;
  request.table = TableReference{
      .project_id = dataset.project_id,
      .dataset_id = dataset.dataset_id,
      .table_id = table.table_reference().table_id(),
  };
  if (body.contains("externalDataConfiguration") &&
      (table.has_view() || table.has_materialized_view())) {
    throw ApiError::Invalid("Cannot combine an external table and a view");
  }
  if (table.has_materialized_view()) {
    if (table.has_view()) {
      throw ApiError::Invalid("Cannot specify both view and materializedView");
    }
    request.materialized = true;
    request.view = body["materializedView"];
  } else if (table.has_view()) {
    if (table.has_table_constraints()) {
      throw ApiError::Invalid("Cannot specify the table constraints when creating view.");
    }
    request.view = body["view"];
  } else {
    request.schema = SchemaFromJson(body.value("schema", json::object()));
  }
  request.metadata = TableMetadataFromJson(body);
  if (request.metadata.external_data) {
    const auto& config = body["externalDataConfiguration"];
    if (config.contains("schema") && !config["schema"].is_null()) {
      auto external_schema = SchemaFromJson(config["schema"]);
      if (body.contains("schema") && !body["schema"].is_null() &&
          !SameSchema(request.schema, external_schema)) {
        throw ApiError::Invalid(
            "When defining a table with an ExternalDataConfiguration, a schema must be present "
            "on either the Table or the ExternalDataConfiguration. If the schema is present on "
            "both, the schemas must be the same.");
      }
      request.schema = std::move(external_schema);
    }
    SetModesGiven(request.schema);
  }
  return request;
}

Routine ParseRoutine(const DatasetReference& dataset, const json& body, std::string_view method) {
  json resource = CheckResource(body, "Routine", RoutineFields());
  const auto routine = ToProto<v2::Routine>(resource);
  resource.erase("routineReference");
  const v2::RoutineReference& reference = routine.routine_reference();
  if (reference.project_id().empty() || reference.dataset_id().empty() ||
      reference.routine_id().empty()) {
    throw ApiError::Invalid(
        "Project ID, dataset ID and routine ID cannot be missing or empty in call to " +
        std::string(method));
  }
  if (reference.project_id() != dataset.project_id ||
      reference.dataset_id() != dataset.dataset_id) {
    throw ApiError::Invalid("Routine reference does not match the request path");
  }
  const bool procedure = routine.routine_type() == v2::Routine::PROCEDURE;
  const bool tvf = routine.routine_type() == v2::Routine::TABLE_VALUED_FUNCTION;
  if (!procedure && !tvf && routine.routine_type() != v2::Routine::SCALAR_FUNCTION) {
    throw ApiError::Unsupported(
        "routines other than SQL scalar functions, table functions and procedures");
  }
  if (!procedure && routine.has_strict_mode()) {
    throw ApiError::Unsupported("strictMode on functions");
  }
  if (routine.has_return_type() && (procedure || tvf)) {
    throw ApiError::Invalid("Only scalar functions can have returnType");
  }
  if (routine.has_return_table_type() && !tvf) {
    throw ApiError::Invalid("Only table functions can have returnTableType");
  }
  const bool python = routine.language() == v2::Routine::PYTHON;
  if (!python && routine.language() != v2::Routine::SQL &&
      routine.language() != v2::Routine::LANGUAGE_UNSPECIFIED) {
    throw ApiError::Unsupported("non-SQL routines other than Python scalar functions");
  }
  resource["language"] = python ? "PYTHON" : "SQL";
  if (python) {
    if (procedure || tvf || !routine.has_return_type() ||
        routine.external_runtime_options().runtime_version() != "python-3.11") {
      throw ApiError::Unsupported("Python routines without a fixed result and python-3.11 runtime");
    }
    if (routine.python_options().entry_point().empty()) {
      throw ApiError::Unsupported("Python routines without an entryPoint");
    }
  } else if (routine.has_external_runtime_options() || routine.has_python_options()) {
    throw ApiError::Unsupported("Python options on SQL routines");
  }
  if (routine.definition_body().empty()) {
    throw ApiError::Invalid("Required parameter is missing: definitionBody");
  }
  if (!resource.contains("arguments")) {
    resource["arguments"] = json::array();
  }
  for (int i = 0; i < routine.arguments_size(); ++i) {
    const v2::Routine::Argument& argument = routine.arguments(i);
    // BigQuery reads an UNSPECIFIED enum as one left out, and returns none.
    if (argument.mode() == v2::Routine::Argument::MODE_UNSPECIFIED) {
      resource["arguments"][i].erase("mode");
    }
    if (argument.argument_kind() == v2::Routine::Argument::ARGUMENT_KIND_UNSPECIFIED) {
      resource["arguments"][i].erase("argumentKind");
    }
    if (argument.name().empty()) {
      throw ApiError::Invalid("Invalid routine argument");
    }
    // Procedures take every mode.
    if (argument.mode() != v2::Routine::Argument::MODE_UNSPECIFIED && !procedure) {
      throw ApiError::Unsupported("routine argument field mode");
    }
    const v2::Routine::Argument::ArgumentKind kind = argument.argument_kind();
    if ((procedure || tvf) && kind == v2::Routine::Argument::ANY_TYPE) {
      throw ApiError::Unsupported("procedure or TVF arguments of ANY TYPE");
    }
    if (kind != v2::Routine::Argument::ARGUMENT_KIND_UNSPECIFIED &&
        kind != v2::Routine::Argument::FIXED_TYPE && kind != v2::Routine::Argument::ANY_TYPE) {
      throw ApiError::Unsupported("routine argument kind " +
                                  v2::Routine::Argument::ArgumentKind_Name(kind));
    }
    if (kind == v2::Routine::Argument::ANY_TYPE && argument.has_data_type()) {
      throw ApiError::Invalid("ANY_TYPE arguments cannot have dataType");
    }
  }
  return {
      .reference =
          {
              .project_id = dataset.project_id,
              .dataset_id = dataset.dataset_id,
              .routine_id = reference.routine_id(),
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
  const auto table = ToProto<v2::Table>(body);
  if (table.has_materialized_view()) {
    throw ApiError::Unsupported("updating materializedView");
  }
  if (body.contains("externalDataConfiguration")) {
    throw ApiError::Unsupported("updating externalDataConfiguration");
  }
  TableUpdateRequest request{.resource = body, .schema = std::nullopt, .view = std::nullopt};
  // As BigQuery answers, a patch that sets the schema to null keeps it; the emulator keeps a view
  // set to null too.
  if (table.has_schema()) {
    request.schema = SchemaFromJson(body["schema"]);
  }
  if (table.has_view()) {
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
