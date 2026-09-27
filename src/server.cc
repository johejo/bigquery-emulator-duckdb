#include "src/server.h"

#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "httplib.h"
#include "nlohmann/json.hpp"
#include "src/api_error.h"
#include "src/discovery_document.h"
#include "src/emulator.h"
#include "src/field_schema.h"
#include "src/query_parameters.h"

namespace bigquery_emulator_duckdb {
namespace {

using nlohmann::json;

constexpr int64_t kDefaultMaxResults = 100000;

// The path prefix of the BigQuery REST API, below the API root.
const std::string kApiPrefix = "/bigquery/v2";  // NOLINT(cert-err58-cpp)

json ErrorBody(const ApiError& error) {
  const char* status = "INVALID_ARGUMENT";
  if (error.http_status() == 404) {
    status = "NOT_FOUND";
  } else if (error.http_status() == 409) {
    status = "ALREADY_EXISTS";
  }
  return json{{"error",
               {{"code", error.http_status()},
                {"message", error.what()},
                {"errors", json::array({json{{"message", error.what()},
                                             {"domain", "global"},
                                             {"reason", error.reason()}}})},
                {"status", status}}}};
}

json ErrorProto(const ApiError& error) {
  return json{{"reason", error.reason()}, {"location", "query"}, {"message", error.what()}};
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

std::pair<json, std::string> ParseMultipartUpload(const httplib::Request& request) {
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

struct TemporaryUpload {
  std::string path;
  ~TemporaryUpload() {
    if (!path.empty()) {
      std::error_code ignored;
      std::filesystem::remove(path, ignored);
    }
  }
};

std::string Param(const httplib::Request& request, const char* name) {
  return request.path_params.at(name);
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

bool QueryParamBool(const httplib::Request& request, const char* name) {
  return request.has_param(name) && request.get_param_value(name) == "true";
}

std::optional<DatasetReference> ParseDefaultDataset(const json& config) {
  if (!config.contains("defaultDataset")) {
    return std::nullopt;
  }
  const json& dataset = config["defaultDataset"];
  return DatasetReference{dataset.value("projectId", ""), dataset.value("datasetId", "")};
}

// jobs.query takes the query configuration as the request body and jobs.insert takes it as
// configuration.query, but the fields this emulator reads are spelled the same in both.
QueryRequest ToQueryRequest(const std::string& project_id, const json& config) {
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

// Derives JobStatistics2.statementType from the leading keywords of the query.
std::string StatementType(const std::string& query) {
  std::vector<std::string> words;
  std::string word;
  for (const char c : query) {
    if (std::isalpha(static_cast<unsigned char>(c)) != 0) {
      word += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    } else if (!word.empty()) {
      words.push_back(word);
      word.clear();
      if (words.size() == 3) {
        break;
      }
    }
  }
  if (!word.empty() && words.size() < 3) {
    words.push_back(word);
  }
  if (words.empty()) {
    return "SELECT";
  }
  const std::string& first = words[0];
  if (first == "CREATE" || first == "DROP" || first == "ALTER") {
    for (size_t i = 1; i < words.size(); ++i) {
      if (words[i] == "TABLE" || words[i] == "VIEW" || words[i] == "SCHEMA" ||
          words[i] == "FUNCTION") {
        return first + "_" + words[i];
      }
    }
    return first + "_TABLE";
  }
  if (first == "INSERT" || first == "UPDATE" || first == "DELETE" || first == "MERGE" ||
      first == "TRUNCATE") {
    return first;
  }
  return "SELECT";
}

json JobReference(const Job& job) {
  return json{{"projectId", job.project_id}, {"jobId", job.job_id}, {"location", job.location}};
}

json JobStatus(const Job& job) {
  json status = {{"state", "DONE"}};
  if (job.error.has_value()) {
    status["errorResult"] = ErrorProto(*job.error);
    status["errors"] = json::array({ErrorProto(*job.error)});
  }
  return status;
}

json JobStatistics(const Job& job) {
  if (job.is_copy) {
    return json{{"creationTime", std::to_string(job.creation_time_ms)},
                {"startTime", std::to_string(job.creation_time_ms)},
                {"endTime", std::to_string(job.end_time_ms)},
                {"copy", {{"copiedRows", std::to_string(job.output_rows)}}}};
  }
  if (job.is_load) {
    return json{{"creationTime", std::to_string(job.creation_time_ms)},
                {"startTime", std::to_string(job.creation_time_ms)},
                {"endTime", std::to_string(job.end_time_ms)},
                {"load", {{"outputRows", std::to_string(job.output_rows)}}}};
  }
  json query_statistics = {{"totalBytesProcessed", "0"},
                           {"totalBytesBilled", "0"},
                           {"cacheHit", false},
                           {"statementType", StatementType(job.query)}};
  if (job.result.has_value() && job.result->affected_rows >= 0) {
    query_statistics["numDmlAffectedRows"] = std::to_string(job.result->affected_rows);
  }
  // A dry run reports what the query would return; that schema is all it produces.
  if (job.dry_run && job.result.has_value() && job.result->has_rows) {
    query_statistics["schema"] = job.result->SchemaToJson();
  }
  return json{{"creationTime", std::to_string(job.creation_time_ms)},
              {"startTime", std::to_string(job.creation_time_ms)},
              {"endTime", std::to_string(job.end_time_ms)},
              {"totalBytesProcessed", "0"},
              {"query", std::move(query_statistics)}};
}

json TableReferenceJson(const TableReference& table) {
  return json{{"projectId", table.project_id},
              {"datasetId", table.dataset_id},
              {"tableId", table.table_id}};
}

json JobResource(const Job& job) {
  if (job.is_copy) {
    json copy = job.copy_configuration;
    auto complete = [&job](json& table) {
      if (table.value("projectId", "").empty()) table["projectId"] = job.project_id;
    };
    complete(copy["destinationTable"]);
    if (copy.contains("sourceTable")) complete(copy["sourceTable"]);
    if (copy.contains("sourceTables")) {
      for (json& table : copy["sourceTables"]) complete(table);
    }
    copy["createDisposition"] =
        job.create_disposition.empty() ? "CREATE_IF_NEEDED" : job.create_disposition;
    copy["writeDisposition"] =
        job.write_disposition.empty() ? "WRITE_EMPTY" : job.write_disposition;
    return json{{"kind", "bigquery#job"},
                {"etag", ""},
                {"id", job.project_id + ":" + job.location + "." + job.job_id},
                {"selfLink", ""},
                {"jobReference", JobReference(job)},
                {"configuration", {{"jobType", "COPY"}, {"copy", std::move(copy)}}},
                {"status", JobStatus(job)},
                {"statistics", JobStatistics(job)}};
  }
  if (job.is_load) {
    if (!job.destination_table.has_value()) {
      throw std::logic_error("Load job is missing its destination table");
    }
    json load = job.load_configuration;
    TableReference destination = *job.destination_table;
    if (destination.project_id.empty()) destination.project_id = job.project_id;
    load["destinationTable"] = TableReferenceJson(destination);
    return json{{"kind", "bigquery#job"},
                {"etag", ""},
                {"id", job.project_id + ":" + job.location + "." + job.job_id},
                {"selfLink", ""},
                {"jobReference", JobReference(job)},
                {"configuration", {{"jobType", "LOAD"}, {"load", std::move(load)}}},
                {"status", JobStatus(job)},
                {"statistics", JobStatistics(job)}};
  }
  json query{{"query", job.query}, {"useLegacySql", false}};
  if (job.destination_table.has_value()) {
    TableReference destination = *job.destination_table;
    if (destination.project_id.empty()) {
      destination.project_id = job.project_id;
    }
    query["destinationTable"] = TableReferenceJson(destination);
    query["createDisposition"] =
        job.create_disposition.empty() ? "CREATE_IF_NEEDED" : job.create_disposition;
    query["writeDisposition"] =
        job.write_disposition.empty() ? "WRITE_EMPTY" : job.write_disposition;
  }
  return json{{"kind", "bigquery#job"},
              {"etag", ""},
              {"id", job.project_id + ":" + job.location + "." + job.job_id},
              {"selfLink", ""},
              {"jobReference", JobReference(job)},
              {"configuration",
               {{"jobType", "QUERY"}, {"dryRun", job.dry_run}, {"query", std::move(query)}}},
              {"status", JobStatus(job)},
              {"statistics", JobStatistics(job)}};
}

json JobListEntry(const Job& job, bool full) {
  json entry = {{"kind", "bigquery#job"},
                {"id", job.project_id + ":" + job.location + "." + job.job_id},
                {"jobReference", JobReference(job)},
                {"state", "DONE"},
                {"configuration", JobResource(job)["configuration"]},
                {"statistics", JobStatistics(job)}};
  if (job.error.has_value()) {
    entry["errorResult"] = ErrorProto(*job.error);
  }
  if (full) {
    entry["status"] = JobStatus(job);
  }
  return entry;
}

// The slice of a job's rows that one response carries.
struct ResultPage {
  int64_t start_index = 0;
  int64_t max_results = 0;
  // Clients that ask for formatOptions.useInt64Timestamp get TIMESTAMP values as epoch
  // microseconds instead of the default decimal seconds.
  bool int64_timestamps = false;
};

// Copies the rows of `result` in [begin, end) into a response, in the timestamp encoding the
// request asked for.
json RowsForResponse(const QueryResult& result, int64_t begin, int64_t end, bool int64_timestamps) {
  const bool as_seconds = !int64_timestamps && HasTimestampField(result.schema);
  json rows = json::array();
  for (int64_t i = begin; i < end; ++i) {
    rows.push_back(as_seconds ? TimestampsAsSeconds(result.schema, result.rows[i])
                              : result.rows[i]);
  }
  return rows;
}

// Fills the result fields shared by jobs.query and jobs.getQueryResults responses.
void AddQueryResults(const Job& job, const ResultPage& page, json& response) {
  response["jobReference"] = JobReference(job);
  response["jobComplete"] = true;
  response["totalBytesProcessed"] = "0";
  response["cacheHit"] = false;
  if (job.error.has_value()) {
    response["errors"] = json::array({ErrorProto(*job.error)});
    return;
  }
  // Job sets exactly one of `result` and `error`, but nothing in the type system says so.
  if (!job.result.has_value()) {
    throw ApiError::Internal("Job has neither a result nor an error");
  }
  const QueryResult& result = *job.result;
  if (result.affected_rows >= 0) {
    response["numDmlAffectedRows"] = std::to_string(result.affected_rows);
  }
  if (!result.has_rows) {
    response["totalRows"] = "0";
    return;
  }
  response["schema"] = result.SchemaToJson();
  response["totalRows"] = std::to_string(result.rows.size());
  const int64_t total = static_cast<int64_t>(result.rows.size());
  const int64_t begin = std::clamp<int64_t>(page.start_index, 0, total);
  const int64_t end = std::min(total, begin + std::max<int64_t>(page.max_results, 0));
  response["rows"] = RowsForResponse(result, begin, end, page.int64_timestamps);
  if (end < total) {
    response["pageToken"] = std::to_string(end);
  }
}

json DryRunQueryResponse(const Job& job) {
  json response = {{"kind", "bigquery#queryResponse"},
                   {"jobComplete", true},
                   {"totalBytesProcessed", "0"},
                   {"cacheHit", false}};
  if (job.error.has_value()) {
    throw *job.error;
  }
  if (job.result.has_value() && job.result->has_rows) {
    response["schema"] = job.result->SchemaToJson();
  }
  response["totalRows"] = "0";
  return response;
}

bool Int64Timestamps(const httplib::Request& request) {
  return QueryParamBool(request, "formatOptions.useInt64Timestamp");
}

int64_t StartIndex(const httplib::Request& request) {
  if (request.has_param("pageToken")) {
    return QueryParamInt(request, "pageToken", 0);
  }
  return QueryParamInt(request, "startIndex", 0);
}

json DatasetResource(const DatasetReference& dataset) {
  return json{
      {"kind", "bigquery#dataset"},
      {"etag", ""},
      {"id", dataset.project_id + ":" + dataset.dataset_id},
      {"datasetReference", {{"projectId", dataset.project_id}, {"datasetId", dataset.dataset_id}}},
      {"location", "US"}};
}

json TableResource(const TableInfo& info) {
  return json{{"kind", "bigquery#table"},
              {"etag", ""},
              {"id", info.reference.project_id + ":" + info.reference.dataset_id + "." +
                         info.reference.table_id},
              {"tableReference", TableReferenceJson(info.reference)},
              {"schema", SchemaToJson(info.schema)},
              {"type", "TABLE"},
              {"numRows", std::to_string(info.num_rows)},
              {"numBytes", "0"},
              {"location", "US"}};
}

}  // namespace

class Server::Impl {
 public:
  Impl(Emulator& emulator, Server& server) : emulator_(emulator), server_(server) {
    http_.set_logger([](const httplib::Request& request, const httplib::Response& response) {
      std::cerr << request.method << " " << request.path << " -> " << response.status << '\n';
    });
    http_.set_exception_handler([](const httplib::Request&, httplib::Response& response,
                                   const std::exception_ptr& exception) {
      std::string message;
      try {
        std::rethrow_exception(exception);
      } catch (const std::exception& error) {
        message = error.what();
      } catch (...) {
        message = "Internal error";
      }
      response.status = 500;
      response.set_content(ErrorBody(ApiError::Internal(message)).dump(), "application/json");
    });
    RegisterRoutes();
  }

  httplib::Server& http() { return http_; }

 private:
  using Handler = std::function<json(const httplib::Request&, httplib::Response&)>;

  // Every resource is served twice: under the /bigquery/v2 prefix that the REST API uses, and
  // directly under the root. A client pointed at the emulator with an endpoint override
  // replaces the whole API base path, prefix included, and then asks for /projects/... —
  // option.WithEndpoint in the Go client works that way.
  void Get(const std::string& path, httplib::Server::Handler handler) {
    http_.Get(kApiPrefix + path, handler);
    http_.Get(path, std::move(handler));
  }
  void Post(const std::string& path, httplib::Server::Handler handler) {
    http_.Post(kApiPrefix + path, handler);
    http_.Post(path, std::move(handler));
  }
  void Delete(const std::string& path, httplib::Server::Handler handler) {
    http_.Delete(kApiPrefix + path, handler);
    http_.Delete(path, std::move(handler));
  }

  // Wraps a handler so that it returns JSON and maps ApiError to BigQuery's error format.
  httplib::Server::Handler Json(Handler handler) {
    return [handler = std::move(handler)](const httplib::Request& request,
                                          httplib::Response& response) {
      try {
        const json body = handler(request, response);
        response.set_content(body.dump(), "application/json");
      } catch (const ApiError& error) {
        response.status = error.http_status();
        response.set_content(ErrorBody(error).dump(), "application/json");
      }
    };
  }

  void RegisterRoutes() {
    // Patterns without path parameters are regular expressions in cpp-httplib, so "$" needs
    // escaping.
    http_.Get(R"(/\$discovery/rest)", [this](const httplib::Request&, httplib::Response& response) {
      static const json* const kDocument = new json(json::parse(DiscoveryDocument()));
      json document = *kDocument;
      const std::string root = server_.root_url() + "/";
      document["rootUrl"] = root;
      document["mtlsRootUrl"] = root;
      document["baseUrl"] = root + "bigquery/v2/";
      response.set_content(document.dump(), "application/json");
    });

    // jobs
    Post("/projects/:project/queries",
         Json([this](const httplib::Request& request, httplib::Response&) {
           const json body = ParseBody(request);
           QueryRequest query_request = ToQueryRequest(Param(request, "project"), body);
           query_request.dry_run = body.value("dryRun", false);
           const auto job = emulator_.RunQuery(query_request);
           if (job->dry_run) {
             // A dry run creates no job, so its response has no job reference either.
             return DryRunQueryResponse(*job);
           }
           json response = {{"kind", "bigquery#queryResponse"}};
           AddQueryResults(
               *job,
               {.max_results = body.value("maxResults", kDefaultMaxResults),
                .int64_timestamps =
                    body.value("formatOptions", json::object()).value("useInt64Timestamp", false)},
               response);
           return response;
         }));
    Get("/projects/:project/queries/:job",
        Json([this](const httplib::Request& request, httplib::Response&) {
          const auto job = emulator_.GetJob(Param(request, "project"), Param(request, "job"));
          json response = {{"kind", "bigquery#getQueryResultsResponse"}, {"etag", ""}};
          AddQueryResults(*job,
                          {.start_index = StartIndex(request),
                           .max_results = QueryParamInt(request, "maxResults", kDefaultMaxResults),
                           .int64_timestamps = Int64Timestamps(request)},
                          response);
          return response;
        }));
    const auto insert_job = Json([this](const httplib::Request& request,
                                        httplib::Response& response) {
      if ((request.has_param("uploadType") &&
           request.get_param_value("uploadType") == "resumable") ||
          (request.has_param("upload_protocol") &&
           request.get_param_value("upload_protocol") == "resumable")) {
        const json metadata = ParseBody(request);
        std::string id;
        {
          std::lock_guard<std::mutex> lock(uploads_mutex_);
          id = std::to_string(next_upload_id_++);
          uploads_[id] = metadata;
        }
        response.set_header("Location", server_.root_url() +
                                            "/resumable/upload/bigquery/v2/projects/" +
                                            Param(request, "project") + "/jobs/" + id);
        return json::object();
      }
      TemporaryUpload upload;
      json body;
      if (request.get_header_value("Content-Type").find("multipart/related") != std::string::npos) {
        auto [metadata, content] = ParseMultipartUpload(request);
        body = std::move(metadata);
        char pattern[] = "/tmp/bigquery-upload-XXXXXX";
        const int fd = mkstemp(pattern);
        if (fd < 0) throw ApiError::Internal("Could not create upload temporary file");
        close(fd);
        upload.path = pattern;
        std::ofstream stream(upload.path, std::ios::binary);
        stream.write(content.data(), static_cast<std::streamsize>(content.size()));
        if (!stream) throw ApiError::Internal("Could not write upload temporary file");
        stream.close();
        body["configuration"]["load"]["sourceUris"] = json::array({upload.path});
      } else {
        body = ParseBody(request);
      }
      const json config = body.value("configuration", json::object());
      if (config.contains("load")) {
        const json& load = config.at("load");
        if (!load.is_object() || !load.contains("destinationTable") ||
            !load.at("destinationTable").is_object()) {
          throw ApiError::Invalid("Invalid destination table");
        }
        const json& table = load.at("destinationTable");
        LoadRequest load_request;
        load_request.project_id = Param(request, "project");
        load_request.job_id = body.value("jobReference", json::object()).value("jobId", "");
        load_request.destination_table = TableReference{
            table.value("projectId", ""), table.value("datasetId", ""), table.value("tableId", "")};
        if (load_request.destination_table.dataset_id.empty() ||
            load_request.destination_table.table_id.empty()) {
          throw ApiError::Invalid("Invalid destination table");
        }
        load_request.configuration = load;
        return JobResource(*emulator_.RunLoad(load_request));
      }
      if (config.contains("copy")) {
        const json& copy = config.at("copy");
        auto parse_table = [](const json& table) {
          if (!table.is_object()) throw ApiError::Invalid("Invalid table reference");
          TableReference result{table.value("projectId", ""), table.value("datasetId", ""),
                                table.value("tableId", "")};
          if (result.dataset_id.empty() || result.table_id.empty()) {
            throw ApiError::Invalid("Invalid table reference");
          }
          return result;
        };
        if (!copy.is_object() || !copy.contains("destinationTable")) {
          throw ApiError::Invalid("Invalid destination table");
        }
        CopyRequest copy_request;
        copy_request.project_id = Param(request, "project");
        copy_request.job_id = body.value("jobReference", json::object()).value("jobId", "");
        copy_request.destination_table = parse_table(copy.at("destinationTable"));
        if (copy.contains("sourceTable") == copy.contains("sourceTables")) {
          throw ApiError::Invalid("Specify sourceTable or sourceTables");
        }
        if (copy.contains("sourceTable")) {
          copy_request.source_tables.push_back(parse_table(copy.at("sourceTable")));
        } else {
          if (!copy.at("sourceTables").is_array() || copy.at("sourceTables").empty()) {
            throw ApiError::Invalid("sourceTables is required");
          }
          for (const json& table : copy.at("sourceTables")) {
            copy_request.source_tables.push_back(parse_table(table));
          }
        }
        copy_request.configuration = copy;
        return JobResource(*emulator_.RunCopy(copy_request));
      }
      if (!config.contains("query")) {
        throw ApiError::Invalid("Only query, load, and copy jobs are supported");
      }
      QueryRequest query_request = ToQueryRequest(Param(request, "project"), config["query"]);
      query_request.job_id = body.value("jobReference", json::object()).value("jobId", "");
      query_request.dry_run = config.value("dryRun", false);
      const json& query_config = config["query"];
      if (query_config.contains("destinationTable")) {
        const json& table = query_config["destinationTable"];
        query_request.destination_table = TableReference{
            table.value("projectId", ""), table.value("datasetId", ""), table.value("tableId", "")};
        if (query_request.destination_table->dataset_id.empty() ||
            query_request.destination_table->table_id.empty()) {
          throw ApiError::Invalid("Invalid destination table");
        }
      }
      query_request.create_disposition = query_config.value("createDisposition", "");
      query_request.write_disposition = query_config.value("writeDisposition", "");
      return JobResource(*emulator_.RunQuery(query_request));
    });
    Post("/projects/:project/jobs", insert_job);
    http_.Post("/upload/bigquery/v2/projects/:project/jobs", insert_job);
    http_.Post("/resumable/upload/bigquery/v2/projects/:project/jobs",
               Json([this](const httplib::Request& request, httplib::Response& response) {
                 const json body = ParseBody(request);
                 if (!body.value("configuration", json::object()).contains("load")) {
                   throw ApiError::Invalid("Resumable upload requires a load job");
                 }
                 std::string id;
                 {
                   std::lock_guard<std::mutex> lock(uploads_mutex_);
                   id = std::to_string(next_upload_id_++);
                   uploads_[id] = body;
                 }
                 response.set_header("Location", server_.root_url() +
                                                     "/resumable/upload/bigquery/v2/projects/" +
                                                     Param(request, "project") + "/jobs/" + id);
                 return json::object();
               }));
    http_.Put("/resumable/upload/bigquery/v2/projects/:project/jobs/:upload",
              Json([this](const httplib::Request& request, httplib::Response&) {
                json body;
                {
                  std::lock_guard<std::mutex> lock(uploads_mutex_);
                  const auto it = uploads_.find(Param(request, "upload"));
                  if (it == uploads_.end()) throw ApiError::NotFound("Upload session not found");
                  body = std::move(it->second);
                  uploads_.erase(it);
                }
                TemporaryUpload upload;
                char pattern[] = "/tmp/bigquery-upload-XXXXXX";
                const int fd = mkstemp(pattern);
                if (fd < 0) throw ApiError::Internal("Could not create upload temporary file");
                close(fd);
                upload.path = pattern;
                std::ofstream stream(upload.path, std::ios::binary);
                stream.write(request.body.data(),
                             static_cast<std::streamsize>(request.body.size()));
                if (!stream) throw ApiError::Internal("Could not write upload temporary file");
                stream.close();
                json& config = body["configuration"]["load"];
                config["sourceUris"] = json::array({upload.path});
                const json& table = config.at("destinationTable");
                LoadRequest load_request;
                load_request.project_id = Param(request, "project");
                load_request.job_id = body.value("jobReference", json::object()).value("jobId", "");
                load_request.destination_table =
                    TableReference{table.value("projectId", ""), table.value("datasetId", ""),
                                   table.value("tableId", "")};
                load_request.configuration = config;
                return JobResource(*emulator_.RunLoad(load_request));
              }));
    Get("/projects/:project/jobs",
        Json([this](const httplib::Request& request, httplib::Response&) {
          const std::string projection =
              request.has_param("projection") ? request.get_param_value("projection") : "full";
          if (projection != "full" && projection != "minimal") {
            throw ApiError::Invalid("Invalid value for projection");
          }
          const std::string state =
              request.has_param("stateFilter") ? request.get_param_value("stateFilter") : "";
          if (!state.empty() && state != "done" && state != "pending" && state != "running") {
            throw ApiError::Invalid("Invalid value for stateFilter");
          }
          const int64_t max_results = QueryParamInt(request, "maxResults", 50);
          const int64_t offset = QueryParamInt(request, "pageToken", 0);
          const int64_t min_time = QueryParamInt(request, "minCreationTime", 0);
          const int64_t max_time = QueryParamInt(request, "maxCreationTime", INT64_MAX);
          if (max_results <= 0 || offset < 0 || min_time < 0 || max_time < 0) {
            throw ApiError::Invalid("Invalid jobs.list parameter");
          }
          json response = {{"kind", "bigquery#jobList"}, {"etag", ""}};
          json jobs = json::array();
          int64_t index = 0;
          const bool parent_filter = request.has_param("parentJobId");
          for (const auto& job : emulator_.ListJobs(Param(request, "project"))) {
            if (parent_filter || (state != "" && state != "done") ||
                job->creation_time_ms < min_time || job->creation_time_ms > max_time) {
              continue;
            }
            if (index++ < offset) {
              continue;
            }
            if (static_cast<int64_t>(jobs.size()) == max_results) {
              response["nextPageToken"] = std::to_string(index - 1);
              break;
            }
            jobs.push_back(JobListEntry(*job, projection == "full"));
          }
          if (!jobs.empty()) {
            response["jobs"] = std::move(jobs);
          }
          return response;
        }));
    Get("/projects/:project/jobs/:job",
        Json([this](const httplib::Request& request, httplib::Response&) {
          return JobResource(*emulator_.GetJob(Param(request, "project"), Param(request, "job")));
        }));
    Post("/projects/:project/jobs/:job/cancel",
         Json([this](const httplib::Request& request, httplib::Response&) {
           return json{{"kind", "bigquery#jobCancelResponse"},
                       {"job", JobResource(*emulator_.GetJob(Param(request, "project"),
                                                             Param(request, "job")))}};
         }));
    Delete("/projects/:project/jobs/:job/delete",
           Json([this](const httplib::Request& request, httplib::Response& response) {
             emulator_.DeleteJob(Param(request, "project"), Param(request, "job"));
             response.status = 204;
             return json::object();
           }));

    // datasets
    Get("/projects/:project/datasets",
        Json([this](const httplib::Request& request, httplib::Response&) {
          const std::string project = Param(request, "project");
          json datasets = json::array();
          for (const std::string& dataset_id : emulator_.ListDatasets(project)) {
            datasets.push_back(DatasetResource(DatasetReference{project, dataset_id}));
          }
          return json{
              {"kind", "bigquery#datasetList"}, {"etag", ""}, {"datasets", std::move(datasets)}};
        }));
    Post("/projects/:project/datasets",
         Json([this](const httplib::Request& request, httplib::Response&) {
           const json body = ParseBody(request);
           const json reference = body.value("datasetReference", json::object());
           if (!reference.contains("datasetId")) {
             throw ApiError::Invalid("Required parameter is missing: datasetId");
           }
           const DatasetReference dataset{Param(request, "project"),
                                          reference["datasetId"].get<std::string>()};
           emulator_.CreateDataset(dataset);
           return DatasetResource(dataset);
         }));
    Get("/projects/:project/datasets/:dataset",
        Json([this](const httplib::Request& request, httplib::Response&) {
          const DatasetReference dataset{Param(request, "project"), Param(request, "dataset")};
          emulator_.GetDataset(dataset);
          return DatasetResource(dataset);
        }));
    Delete("/projects/:project/datasets/:dataset",
           Json([this](const httplib::Request& request, httplib::Response& response) {
             emulator_.DeleteDataset(
                 DatasetReference{Param(request, "project"), Param(request, "dataset")},
                 QueryParamBool(request, "deleteContents"));
             response.status = 204;
             return json::object();
           }));

    // tables
    Get("/projects/:project/datasets/:dataset/tables",
        Json([this](const httplib::Request& request, httplib::Response&) {
          const DatasetReference dataset{Param(request, "project"), Param(request, "dataset")};
          json tables = json::array();
          for (const std::string& table_id : emulator_.ListTables(dataset)) {
            const TableReference table{dataset.project_id, dataset.dataset_id, table_id};
            tables.push_back(
                json{{"kind", "bigquery#table"},
                     {"id", table.project_id + ":" + table.dataset_id + "." + table.table_id},
                     {"tableReference", TableReferenceJson(table)},
                     {"type", "TABLE"}});
          }
          return json{{"kind", "bigquery#tableList"},
                      {"etag", ""},
                      {"totalItems", tables.size()},
                      {"tables", std::move(tables)}};
        }));
    Post("/projects/:project/datasets/:dataset/tables",
         Json([this](const httplib::Request& request, httplib::Response&) {
           const json body = ParseBody(request);
           const json reference = body.value("tableReference", json::object());
           if (!reference.contains("tableId")) {
             throw ApiError::Invalid("Required parameter is missing: tableId");
           }
           const TableReference table{Param(request, "project"), Param(request, "dataset"),
                                      reference["tableId"].get<std::string>()};
           emulator_.CreateTable(
               table, body.value("schema", json::object()).value("fields", json::array()));
           return TableResource(emulator_.GetTable(table));
         }));
    Get("/projects/:project/datasets/:dataset/tables/:table",
        Json([this](const httplib::Request& request, httplib::Response&) {
          return TableResource(emulator_.GetTable(TableReference{
              Param(request, "project"), Param(request, "dataset"), Param(request, "table")}));
        }));
    Delete("/projects/:project/datasets/:dataset/tables/:table",
           Json([this](const httplib::Request& request, httplib::Response& response) {
             emulator_.DeleteTable(TableReference{
                 Param(request, "project"), Param(request, "dataset"), Param(request, "table")});
             response.status = 204;
             return json::object();
           }));
    Get("/projects/:project/datasets/:dataset/tables/:table/data",
        Json([this](const httplib::Request& request, httplib::Response&) {
          const TableReference table{Param(request, "project"), Param(request, "dataset"),
                                     Param(request, "table")};
          const int64_t start_index = StartIndex(request);
          const int64_t max_results = QueryParamInt(request, "maxResults", kDefaultMaxResults);
          const QueryResult result = emulator_.ListTableData(table, start_index, max_results);
          const int64_t total = emulator_.GetTable(table).num_rows;
          json response = {
              {"kind", "bigquery#tableDataList"},
              {"etag", ""},
              {"totalRows", std::to_string(total)},
              {"rows", RowsForResponse(result, 0, static_cast<int64_t>(result.rows.size()),
                                       Int64Timestamps(request))}};
          if (start_index + static_cast<int64_t>(result.rows.size()) < total) {
            response["pageToken"] =
                std::to_string(start_index + static_cast<int64_t>(result.rows.size()));
          }
          return response;
        }));
    Post(
        "/projects/:project/datasets/:dataset/tables/:table/insertAll",
        Json([this](const httplib::Request& request, httplib::Response&) {
          const json body = ParseBody(request);
          const TableReference table{Param(request, "project"), Param(request, "dataset"),
                                     Param(request, "table")};
          if (!body.contains("rows")) {
            throw ApiError::Invalid("Required parameter is missing: rows");
          }
          if (body.contains("templateSuffix") && !body["templateSuffix"].is_null() &&
              body["templateSuffix"] != "") {
            throw ApiError::Invalid("templateSuffix is not supported");
          }
          json response = {{"kind", "bigquery#tableDataInsertAllResponse"}};
          const auto errors =
              emulator_.InsertTableData(table, body["rows"], body.value("skipInvalidRows", false),
                                        body.value("ignoreUnknownValues", false));
          if (!errors.empty()) {
            response["insertErrors"] = json::array();
            for (const InsertError& error : errors) {
              response["insertErrors"].push_back(
                  {{"index", error.index},
                   {"errors", json::array({{{"reason", "invalid"}, {"message", error.message}}})}});
            }
          }
          return response;
        }));
  }

  Emulator& emulator_;
  Server& server_;
  httplib::Server http_;
  std::mutex uploads_mutex_;
  std::unordered_map<std::string, json> uploads_;
  uint64_t next_upload_id_ = 1;
};

Server::Server(Emulator& emulator, ServerOptions options)
    : impl_(std::make_unique<Impl>(emulator, *this)), options_(std::move(options)) {}

Server::~Server() = default;

bool Server::Bind() {
  if (options_.port == 0) {
    port_ = impl_->http().bind_to_any_port(options_.host);
    return port_ > 0;
  }
  if (!impl_->http().bind_to_port(options_.host, options_.port)) {
    return false;
  }
  port_ = options_.port;
  return true;
}

bool Server::Serve() { return impl_->http().listen_after_bind(); }

void Server::Stop() { impl_->http().stop(); }

std::string Server::root_url() const {
  const std::string host = options_.host == "0.0.0.0" ? "127.0.0.1" : options_.host;
  return "http://" + host + ":" + std::to_string(port_);
}

}  // namespace bigquery_emulator_duckdb
