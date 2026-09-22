#include "src/server.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "httplib.h"
#include "nlohmann/json.hpp"
#include "src/api_error.h"
#include "src/discovery_document.h"
#include "src/emulator.h"

namespace bigquery_emulator_duckdb {
namespace {

using nlohmann::json;

constexpr int64_t kDefaultMaxResults = 100000;

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

std::string Param(const httplib::Request& request, const char* name) {
  return request.path_params.at(name);
}

int64_t QueryParamInt(const httplib::Request& request, const char* name, int64_t fallback) {
  if (!request.has_param(name)) {
    return fallback;
  }
  try {
    return std::stoll(request.get_param_value(name));
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
  json query_statistics = {{"totalBytesProcessed", "0"},
                           {"totalBytesBilled", "0"},
                           {"cacheHit", false},
                           {"statementType", StatementType(job.query)}};
  if (job.result.has_value() && job.result->affected_rows >= 0) {
    query_statistics["numDmlAffectedRows"] = std::to_string(job.result->affected_rows);
  }
  return json{{"creationTime", std::to_string(job.creation_time_ms)},
              {"startTime", std::to_string(job.creation_time_ms)},
              {"endTime", std::to_string(job.end_time_ms)},
              {"totalBytesProcessed", "0"},
              {"query", std::move(query_statistics)}};
}

json JobResource(const Job& job) {
  return json{{"kind", "bigquery#job"},
              {"etag", ""},
              {"id", job.project_id + ":" + job.location + "." + job.job_id},
              {"selfLink", ""},
              {"jobReference", JobReference(job)},
              {"configuration",
               {{"jobType", "QUERY"}, {"query", {{"query", job.query}, {"useLegacySql", false}}}}},
              {"status", JobStatus(job)},
              {"statistics", JobStatistics(job)}};
}

// Fills the result fields shared by jobs.query and jobs.getQueryResults responses.
void AddQueryResults(const Job& job, int64_t start_index, int64_t max_results, json& response) {
  response["jobReference"] = JobReference(job);
  response["jobComplete"] = true;
  response["totalBytesProcessed"] = "0";
  response["cacheHit"] = false;
  if (job.error.has_value()) {
    response["errors"] = json::array({ErrorProto(*job.error)});
    return;
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
  const int64_t begin = std::clamp<int64_t>(start_index, 0, total);
  const int64_t end = std::min(total, begin + std::max<int64_t>(max_results, 0));
  response["rows"] = json::array();
  for (int64_t i = begin; i < end; ++i) {
    response["rows"].push_back(result.rows[i]);
  }
  if (end < total) {
    response["pageToken"] = std::to_string(end);
  }
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

json TableReferenceJson(const TableReference& table) {
  return json{{"projectId", table.project_id},
              {"datasetId", table.dataset_id},
              {"tableId", table.table_id}};
}

json TableResource(const TableInfo& info) {
  json fields = json::array();
  for (const FieldSchema& field : info.schema) {
    fields.push_back(field.ToJson());
  }
  return json{{"kind", "bigquery#table"},
              {"etag", ""},
              {"id", info.reference.project_id + ":" + info.reference.dataset_id + "." +
                         info.reference.table_id},
              {"tableReference", TableReferenceJson(info.reference)},
              {"schema", {{"fields", std::move(fields)}}},
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
    http_.set_exception_handler(
        [](const httplib::Request&, httplib::Response& response, std::exception_ptr exception) {
          std::string message = "Internal error";
          try {
            std::rethrow_exception(std::move(exception));
          } catch (const std::exception& error) {
            message = error.what();
          } catch (...) {
          }
          response.status = 500;
          response.set_content(ErrorBody(ApiError(500, "internalError", message)).dump(),
                               "application/json");
        });
    RegisterRoutes();
  }

  httplib::Server& http() { return http_; }

 private:
  using Handler = std::function<json(const httplib::Request&, httplib::Response&)>;

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
      json document = json::parse(DiscoveryDocument());
      const std::string root = server_.root_url() + "/";
      document["rootUrl"] = root;
      document["mtlsRootUrl"] = root;
      document["baseUrl"] = root + "bigquery/v2/";
      response.set_content(document.dump(), "application/json");
    });

    // jobs
    http_.Post("/bigquery/v2/projects/:project/queries",
               Json([this](const httplib::Request& request, httplib::Response&) {
                 const json body = ParseBody(request);
                 if (!body.contains("query")) {
                   throw ApiError::Invalid("Required parameter is missing: query");
                 }
                 const auto job =
                     emulator_.RunQuery(Param(request, "project"), body["query"].get<std::string>(),
                                        ParseDefaultDataset(body), "");
                 json response = {{"kind", "bigquery#queryResponse"}};
                 AddQueryResults(*job, 0, body.value("maxResults", kDefaultMaxResults), response);
                 return response;
               }));
    http_.Get("/bigquery/v2/projects/:project/queries/:job",
              Json([this](const httplib::Request& request, httplib::Response&) {
                const auto job = emulator_.GetJob(Param(request, "project"), Param(request, "job"));
                json response = {{"kind", "bigquery#getQueryResultsResponse"}, {"etag", ""}};
                AddQueryResults(*job, StartIndex(request),
                                QueryParamInt(request, "maxResults", kDefaultMaxResults), response);
                return response;
              }));
    http_.Post("/bigquery/v2/projects/:project/jobs",
               Json([this](const httplib::Request& request, httplib::Response&) {
                 const json body = ParseBody(request);
                 const json config = body.value("configuration", json::object());
                 if (!config.contains("query") || !config["query"].contains("query")) {
                   throw ApiError::Invalid("Only query jobs are supported");
                 }
                 const json& query = config["query"];
                 const std::string job_id =
                     body.value("jobReference", json::object()).value("jobId", "");
                 const auto job = emulator_.RunQuery(Param(request, "project"),
                                                     query["query"].get<std::string>(),
                                                     ParseDefaultDataset(query), job_id);
                 return JobResource(*job);
               }));
    http_.Get(
        "/bigquery/v2/projects/:project/jobs/:job",
        Json([this](const httplib::Request& request, httplib::Response&) {
          return JobResource(*emulator_.GetJob(Param(request, "project"), Param(request, "job")));
        }));

    // datasets
    http_.Get("/bigquery/v2/projects/:project/datasets",
              Json([this](const httplib::Request& request, httplib::Response&) {
                const std::string project = Param(request, "project");
                json datasets = json::array();
                for (const std::string& dataset_id : emulator_.ListDatasets(project)) {
                  datasets.push_back(DatasetResource(DatasetReference{project, dataset_id}));
                }
                return json{{"kind", "bigquery#datasetList"},
                            {"etag", ""},
                            {"datasets", std::move(datasets)}};
              }));
    http_.Post("/bigquery/v2/projects/:project/datasets",
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
    http_.Get("/bigquery/v2/projects/:project/datasets/:dataset",
              Json([this](const httplib::Request& request, httplib::Response&) {
                const DatasetReference dataset{Param(request, "project"),
                                               Param(request, "dataset")};
                emulator_.GetDataset(dataset);
                return DatasetResource(dataset);
              }));
    http_.Delete("/bigquery/v2/projects/:project/datasets/:dataset",
                 Json([this](const httplib::Request& request, httplib::Response& response) {
                   emulator_.DeleteDataset(
                       DatasetReference{Param(request, "project"), Param(request, "dataset")},
                       QueryParamBool(request, "deleteContents"));
                   response.status = 204;
                   return json::object();
                 }));

    // tables
    http_.Get("/bigquery/v2/projects/:project/datasets/:dataset/tables",
              Json([this](const httplib::Request& request, httplib::Response&) {
                const DatasetReference dataset{Param(request, "project"),
                                               Param(request, "dataset")};
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
    http_.Post("/bigquery/v2/projects/:project/datasets/:dataset/tables",
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
    http_.Get(
        "/bigquery/v2/projects/:project/datasets/:dataset/tables/:table",
        Json([this](const httplib::Request& request, httplib::Response&) {
          return TableResource(emulator_.GetTable(TableReference{
              Param(request, "project"), Param(request, "dataset"), Param(request, "table")}));
        }));
    http_.Delete(
        "/bigquery/v2/projects/:project/datasets/:dataset/tables/:table",
        Json([this](const httplib::Request& request, httplib::Response& response) {
          emulator_.DeleteTable(TableReference{Param(request, "project"), Param(request, "dataset"),
                                               Param(request, "table")});
          response.status = 204;
          return json::object();
        }));
    http_.Get("/bigquery/v2/projects/:project/datasets/:dataset/tables/:table/data",
              Json([this](const httplib::Request& request, httplib::Response&) {
                const TableReference table{Param(request, "project"), Param(request, "dataset"),
                                           Param(request, "table")};
                const int64_t start_index = StartIndex(request);
                const int64_t max_results =
                    QueryParamInt(request, "maxResults", kDefaultMaxResults);
                const QueryResult result = emulator_.ListTableData(table, start_index, max_results);
                const int64_t total = emulator_.GetTable(table).num_rows;
                json response = {{"kind", "bigquery#tableDataList"},
                                 {"etag", ""},
                                 {"totalRows", std::to_string(total)},
                                 {"rows", result.rows}};
                if (start_index + static_cast<int64_t>(result.rows.size()) < total) {
                  response["pageToken"] =
                      std::to_string(start_index + static_cast<int64_t>(result.rows.size()));
                }
                return response;
              }));
  }

  Emulator& emulator_;
  Server& server_;
  httplib::Server http_;
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
