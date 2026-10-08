#include "src/server.h"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "nlohmann/json.hpp"
#include "src/api_error.h"
#include "src/emulator.h"
#include "src/httplib.h"
#include "src/server/requests.h"
#include "src/server/resources.h"
#include "src/server/routes.h"
#include "src/table_metadata.h"
#include "src/temporary_files.h"

namespace bigquery_emulator_duckdb {
namespace {

using nlohmann::json;

// The path prefix of the BigQuery REST API, below the API root.
const std::string kApiPrefix = "/bigquery/v2";  // NOLINT(cert-err58-cpp)

// The path of resumable upload sessions, below the API root.
constexpr std::string_view kResumablePath = "/resumable/upload/bigquery/v2/projects/";

}  // namespace

using namespace server;

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

  // Serves the discovery document's method `id` with `handler`, which handles the query
  // parameters `accepted`; see CheckQueryParameters.
  //
  // Every method is served twice: under the /bigquery/v2 prefix that the REST API uses, and
  // directly under the root. A client pointed at the emulator with an endpoint override replaces
  // the whole API base path, prefix included, and then asks for /projects/... —
  // option.WithEndpoint in the Go client works that way. Media uploads are served at their own
  // paths.
  void Route(std::string_view id, std::vector<std::string> accepted,
             const httplib::Server::Handler& handler) {
    ApiMethod method = FindApiMethod(id);
    const auto unknown = std::ranges::find_if(
        accepted, [&method](const std::string& name) { return !method.parameters.contains(name); });
    if (unknown != accepted.end()) {
      throw std::logic_error(std::string(id) + " has no query parameter " + *unknown);
    }
    // The cpp-httplib function that serves the method's HTTP method.
    using Serve =
        httplib::Server& (httplib::Server::*)(const std::string&, httplib::Server::Handler);
    Serve serve = nullptr;
    if (method.http_method == "GET") {
      serve = &httplib::Server::Get;
    } else if (method.http_method == "POST") {
      serve = &httplib::Server::Post;
    } else if (method.http_method == "PUT") {
      serve = &httplib::Server::Put;
    } else if (method.http_method == "PATCH") {
      serve = &httplib::Server::Patch;
    } else if (method.http_method == "DELETE") {
      serve = &httplib::Server::Delete;
    } else {
      throw std::logic_error("Cannot route " + std::string(id) + " over " + method.http_method);
    }
    std::vector<std::string> patterns = {kApiPrefix + method.path, method.path};
    patterns.insert(patterns.end(), method.upload_paths.begin(), method.upload_paths.end());
    // Shared by the handlers of every pattern.
    struct CheckedRoute {
      ApiMethod method;
      std::vector<std::string> accepted;
      httplib::Server::Handler handler;
    };
    const auto route = std::make_shared<const CheckedRoute>(
        CheckedRoute{std::move(method), std::move(accepted), handler});
    const httplib::Server::Handler checked = [route, this](const httplib::Request& request,
                                                           httplib::Response& response) {
      try {
        CheckQueryParameters(route->method, route->accepted, request);
        if (request.path_params.contains("projectId")) {
          httplib::Request normalized = request;
          normalized.path_params["projectId"] =
              emulator_.ResolveProject(Param(request, "projectId"));
          route->handler(normalized, response);
        } else {
          route->handler(request, response);
        }
      } catch (const ApiError& error) {
        WriteError(response, error);
        return;
      }
    };
    for (const std::string& pattern : patterns) {
      (http_.*serve)(pattern, checked);
    }
  }

  static void WriteError(httplib::Response& response, const ApiError& error) {
    response.status = error.http_status();
    response.set_content(ErrorBody(error).dump(), "application/json");
  }

  // Runs a handler and maps ApiError to BigQuery's error format.
  static void Respond(httplib::Response& response, const std::function<void()>& handler) {
    try {
      handler();
    } catch (const ApiError& error) {
      WriteError(response, error);
    }
  }

  // Wraps a handler so that it returns JSON.
  static httplib::Server::Handler Json(Handler handler) {
    return [handler = std::move(handler)](const httplib::Request& request,
                                          httplib::Response& response) {
      Respond(response,
              [&] { response.set_content(handler(request, response).dump(), "application/json"); });
    };
  }

  // Wraps a handler that answers 204 No Content. A 204 response has no body; sending one corrupts
  // the next response on the connection.
  static httplib::Server::Handler NoContent(std::function<void(const httplib::Request&)> handler) {
    return [handler = std::move(handler)](const httplib::Request& request,
                                          httplib::Response& response) {
      Respond(response, [&] {
        handler(request);
        response.status = 204;
      });
    };
  }

  void RegisterRoutes() {
    // Patterns without path parameters are regular expressions in cpp-httplib, so "$" needs
    // escaping.
    http_.Get(R"(/\$discovery/rest)", [this](const httplib::Request&, httplib::Response& response) {
      json document = Discovery();
      const std::string root = server_.root_url() + "/";
      document["rootUrl"] = root;
      document["mtlsRootUrl"] = root;
      document["baseUrl"] = root + "bigquery/v2/";
      response.set_content(document.dump(), "application/json");
    });

    Route("bigquery.projects.list", {"maxResults", "pageToken"},
          Json([this](const httplib::Request& request, httplib::Response&) {
            ListPage page = ParseListPage(request);
            if (!request.has_param("maxResults") || request.get_param_value("maxResults").empty()) {
              page.max_results = 50;
            }
            return ProjectList(emulator_.ListProjects(), page);
          }));

    // jobs
    Route("bigquery.jobs.query", {},
          Json([this](const httplib::Request& request, httplib::Response&) {
            const json body = ParseBody(request);
            const auto job = emulator_.RunQuery(ParseQuery(Param(request, "projectId"), body));
            return QueryResponse(*job, ParseQueryPage(body));
          }));
    // Ignores location and timeoutMs, since jobs finish before they are inserted, and
    // formatOptions.timestampOutputFormat.
    Route("bigquery.jobs.getQueryResults",
          {"formatOptions.timestampOutputFormat", "formatOptions.useInt64Timestamp", "location",
           "maxResults", "pageToken", "startIndex", "timeoutMs"},
          Json([this](const httplib::Request& request, httplib::Response&) {
            const auto job = emulator_.GetJob(Param(request, "projectId"), Param(request, "jobId"));
            return GetQueryResultsResponse(*job, ParseResultPage(request));
          }));
    const auto insert_job =
        Json([this](const httplib::Request& request, httplib::Response& response) {
          const std::string project = Param(request, "projectId");
          // The resumable upload path opens a session whatever its query string says.
          if (IsResumableUpload(request) || request.path.starts_with(kResumablePath)) {
            return StartResumableUpload(project, ParseBody(request), response);
          }
          if (IsMultipartUpload(request)) {
            MediaUpload upload = ParseMultipartUpload(request);
            return RunUploadedLoad(ParseLoadInsert(project, upload.metadata), upload.content);
          }
          return std::visit([this](const auto& job) { return JobResource(*Run(job)); },
                            ParseJobInsert(project, ParseBody(request)));
        });
    Route("bigquery.jobs.insert", {}, insert_job);
    http_.Put(std::string(kResumablePath) + ":projectId/jobs/:upload",
              Json([this](const httplib::Request& request, httplib::Response&) {
                return RunUploadedLoad(TakeResumableUpload(request), request.body);
              }));
    // Ignores allUsers: every job is the caller's.
    Route("bigquery.jobs.list",
          {"allUsers", "maxCreationTime", "maxResults", "minCreationTime", "pageToken",
           "parentJobId", "projection", "stateFilter"},
          Json([this](const httplib::Request& request, httplib::Response&) {
            const JobListRequest list = ParseJobList(request);
            return JobList(emulator_.ListJobs(Param(request, "projectId")), list);
          }));
    // jobs.get, jobs.cancel and jobs.delete ignore location, since every job is in the US.
    Route("bigquery.jobs.get", {"location"},
          Json([this](const httplib::Request& request, httplib::Response&) {
            return JobResource(
                *emulator_.GetJob(Param(request, "projectId"), Param(request, "jobId")));
          }));
    Route("bigquery.jobs.cancel", {"location"},
          Json([this](const httplib::Request& request, httplib::Response&) {
            return JobCancelResponse(
                *emulator_.GetJob(Param(request, "projectId"), Param(request, "jobId")));
          }));
    Route("bigquery.jobs.delete", {"location"}, NoContent([this](const httplib::Request& request) {
            emulator_.DeleteJob(Param(request, "projectId"), Param(request, "jobId"));
          }));

    // datasets
    Route("bigquery.datasets.list", {"all", "filter", "maxResults", "pageToken"},
          Json([this](const httplib::Request& request, httplib::Response&) {
            const DatasetFilter filters = ParseDatasetFilter(request);
            const std::string project = Param(request, "projectId");
            std::vector<DatasetListEntry> datasets = emulator_.ListDatasetEntries(project);
            // Datasets whose names start with an underscore are hidden unless `all` asks for them.
            if (!QueryParamBool(request, "all")) {
              std::erase_if(datasets, [](const DatasetListEntry& entry) {
                return entry.dataset_id.starts_with('_');
              });
            }
            std::erase_if(datasets, [&filters](const DatasetListEntry& entry) {
              return std::ranges::any_of(filters, [&entry](const auto& filter) {
                const auto& [key, value] = filter;
                const auto label = entry.metadata.labels.find(key);
                return label == entry.metadata.labels.end() || (value && label->second != *value);
              });
            });
            return DatasetList(project, datasets, ParseListPage(request));
          }));
    // The datasets methods ignore accessPolicyVersion, datasetView and updateMode, which select
    // access controls the emulator does not keep.
    Route("bigquery.datasets.insert", {"accessPolicyVersion"},
          Json([this](const httplib::Request& request, httplib::Response&) {
            const DatasetInsertRequest insert =
                ParseDatasetInsert(Param(request, "projectId"), ParseBody(request));
            emulator_.CreateDataset(insert.dataset, insert.metadata);
            return DatasetResource(insert.dataset, emulator_.GetDataset(insert.dataset));
          }));
    Route("bigquery.datasets.get", {"accessPolicyVersion", "datasetView"},
          Json([this](const httplib::Request& request, httplib::Response&) {
            const DatasetReference dataset = DatasetFromPath(request);
            return DatasetResource(dataset, emulator_.GetDataset(dataset));
          }));
    // datasets.patch and datasets.update differ as tables.patch and tables.update do; see
    // UpdateDatasetMetadata.
    const auto update_dataset = [this](bool patch) {
      return Json([this, patch](const httplib::Request& request, httplib::Response&) {
        const DatasetReference dataset = DatasetFromPath(request);
        const json body = ParseBody(request);
        DatasetMetadata metadata = emulator_.GetDataset(dataset);
        UpdateDatasetMetadata(metadata, body, patch);
        emulator_.UpdateDataset(dataset, metadata);
        return DatasetResource(dataset, emulator_.GetDataset(dataset));
      });
    };
    Route("bigquery.datasets.patch", {"accessPolicyVersion", "updateMode"},
          update_dataset(/*patch=*/true));
    Route("bigquery.datasets.update", {"accessPolicyVersion", "updateMode"},
          update_dataset(/*patch=*/false));
    Route("bigquery.datasets.delete", {"deleteContents"},
          NoContent([this](const httplib::Request& request) {
            emulator_.DeleteDataset(DatasetFromPath(request),
                                    QueryParamBool(request, "deleteContents"));
          }));

    // tables
    Route("bigquery.tables.list", {"maxResults", "pageToken"},
          Json([this](const httplib::Request& request, httplib::Response&) {
            const DatasetReference dataset = DatasetFromPath(request);
            return TableList(dataset, emulator_.ListTableEntries(dataset), ParseListPage(request));
          }));
    Route("bigquery.tables.insert", {},
          Json([this](const httplib::Request& request, httplib::Response&) {
            const TableInsertRequest insert =
                ParseTableInsert(DatasetFromPath(request), ParseBody(request));
            if (insert.view.has_value()) {
              emulator_.CreateView(insert.table, *insert.view, insert.metadata);
            } else {
              emulator_.CreateTable(insert.table, insert.schema, insert.metadata);
            }
            return TableResource(emulator_.GetTable(insert.table));
          }));
    Route("bigquery.tables.get", {"selectedFields", "view"},
          Json([this](const httplib::Request& request, httplib::Response&) {
            const TableGetRequest get = ParseTableGet(request);
            return TableGetResource(emulator_.GetTable(TableFromPath(request), get.storage_stats),
                                    get);
          }));
    // tables.patch and tables.update differ in which fields BigQuery keeps when the request leaves
    // them out; see UpdateTableMetadata. The emulator keeps the schema and view for both.
    const auto update_table = [this](bool patch) {
      return Json([this, patch](const httplib::Request& request, httplib::Response&) {
        const TableReference table = TableFromPath(request);
        const json body = ParseBody(request);
        const TableUpdateRequest update = ParseTableUpdate(body);
        TableMetadata metadata = emulator_.GetTable(table, /*include_row_count=*/false).metadata;
        UpdateTableMetadata(metadata, body, patch);
        emulator_.UpdateTable(table, update.schema, update.view, metadata);
        return TableResource(emulator_.GetTable(table));
      });
    };
    // Both ignore autodetect_schema.
    Route("bigquery.tables.patch", {"autodetect_schema"}, update_table(/*patch=*/true));
    Route("bigquery.tables.update", {"autodetect_schema"}, update_table(/*patch=*/false));
    Route("bigquery.tables.delete", {}, NoContent([this](const httplib::Request& request) {
            emulator_.DeleteTable(TableFromPath(request));
          }));
    // Ignores formatOptions.timestampOutputFormat.
    Route("bigquery.tabledata.list",
          {"formatOptions.timestampOutputFormat", "formatOptions.useInt64Timestamp", "maxResults",
           "pageToken", "selectedFields", "startIndex"},
          Json([this](const httplib::Request& request, httplib::Response&) {
            const TableReference table = TableFromPath(request);
            const ResultPage page = ParseResultPage(request);
            const QueryResult result =
                emulator_.ListTableData(table, page.start_index, page.max_results);
            return TableDataList(result, emulator_.GetTable(table).num_rows, page,
                                 request.get_param_value("selectedFields"));
          }));
    Route("bigquery.tabledata.insertAll", {},
          Json([this](const httplib::Request& request, httplib::Response&) {
            const InsertAllRequest insert = ParseInsertAll(ParseBody(request));
            return InsertAllResponse(emulator_.InsertTableData(TableFromPath(request), insert.rows,
                                                               insert.skip_invalid_rows,
                                                               insert.ignore_unknown_values));
          }));
  }

  std::shared_ptr<const Job> Run(const QueryRequest& request) {
    return emulator_.RunQuery(request);
  }
  std::shared_ptr<const Job> Run(const LoadRequest& request) { return emulator_.RunLoad(request); }
  std::shared_ptr<const Job> Run(const CopyRequest& request) { return emulator_.RunCopy(request); }
  std::shared_ptr<const Job> Run(const ExtractRequest& request) {
    return emulator_.RunExtract(request);
  }

  // Loads `content`, the media of an upload, as `request` describes.
  json RunUploadedLoad(LoadRequest request, std::string_view content) {
    TemporaryFiles files;
    request.load.configuration["sourceUris"] = json::array({files.Write(content)});
    return JobResource(*emulator_.RunLoad(request));
  }

  // Opens a resumable upload session for the load job `metadata` and points the client at it.
  json StartResumableUpload(const std::string& project, const json& metadata,
                            httplib::Response& response) {
    LoadRequest request = ParseLoadInsert(project, metadata);
    std::string id;
    {
      std::scoped_lock lock(uploads_mutex_);
      id = std::to_string(next_upload_id_++);
      uploads_.emplace(id, std::move(request));
    }
    response.set_header("Location",
                        server_.root_url() + std::string(kResumablePath) + project + "/jobs/" + id);
    return json::object();
  }

  // Closes the request's upload session and returns the load job it was opened for.
  LoadRequest TakeResumableUpload(const httplib::Request& request) {
    const std::string project = emulator_.ResolveProject(Param(request, "projectId"));
    const std::string id = Param(request, "upload");
    std::scoped_lock lock(uploads_mutex_);
    const auto it = uploads_.find(id);
    if (it == uploads_.end() || it->second.project_id != project) {
      throw ApiError::NotFound("Upload session not found");
    }
    LoadRequest load = std::move(it->second);
    uploads_.erase(it);
    return load;
  }

  Emulator& emulator_;
  Server& server_;
  httplib::Server http_;
  std::mutex uploads_mutex_;
  std::unordered_map<std::string, LoadRequest> uploads_;
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
