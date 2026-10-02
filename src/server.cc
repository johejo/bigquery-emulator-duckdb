#include "src/server.h"

#include <cstdint>
#include <exception>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "httplib.h"
#include "nlohmann/json.hpp"
#include "src/api_error.h"
#include "src/discovery_document.h"
#include "src/emulator.h"
#include "src/server/internal.h"
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
  void Put(const std::string& path, httplib::Server::Handler handler) {
    http_.Put(kApiPrefix + path, handler);
    http_.Put(path, std::move(handler));
  }
  void Patch(const std::string& path, httplib::Server::Handler handler) {
    http_.Patch(kApiPrefix + path, handler);
    http_.Patch(path, std::move(handler));
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
           const auto job = emulator_.RunQuery(ParseQuery(Param(request, "project"), body));
           return QueryResponse(*job, ParseQueryPage(body));
         }));
    Get("/projects/:project/queries/:job",
        Json([this](const httplib::Request& request, httplib::Response&) {
          const auto job = emulator_.GetJob(Param(request, "project"), Param(request, "job"));
          return GetQueryResultsResponse(*job, ParseResultPage(request));
        }));
    const auto insert_job =
        Json([this](const httplib::Request& request, httplib::Response& response) {
          const std::string project = Param(request, "project");
          if (IsResumableUpload(request)) {
            return StartResumableUpload(project, ParseBody(request), response);
          }
          if (IsMultipartUpload(request)) {
            MediaUpload upload = ParseMultipartUpload(request);
            return RunUploadedLoad(ParseLoadInsert(project, upload.metadata), upload.content);
          }
          return std::visit([this](const auto& job) { return JobResource(*Run(job)); },
                            ParseJobInsert(project, ParseBody(request)));
        });
    Post("/projects/:project/jobs", insert_job);
    http_.Post("/upload/bigquery/v2/projects/:project/jobs", insert_job);
    http_.Post(std::string(kResumablePath) + ":project/jobs",
               Json([this](const httplib::Request& request, httplib::Response& response) {
                 return StartResumableUpload(Param(request, "project"), ParseBody(request),
                                             response);
               }));
    http_.Put(std::string(kResumablePath) + ":project/jobs/:upload",
              Json([this](const httplib::Request& request, httplib::Response&) {
                return RunUploadedLoad(TakeResumableUpload(Param(request, "upload")), request.body);
              }));
    Get("/projects/:project/jobs",
        Json([this](const httplib::Request& request, httplib::Response&) {
          const JobListRequest list = ParseJobList(request);
          return JobList(emulator_.ListJobs(Param(request, "project")), list);
        }));
    Get("/projects/:project/jobs/:job",
        Json([this](const httplib::Request& request, httplib::Response&) {
          return JobResource(*emulator_.GetJob(Param(request, "project"), Param(request, "job")));
        }));
    Post("/projects/:project/jobs/:job/cancel",
         Json([this](const httplib::Request& request, httplib::Response&) {
           return JobCancelResponse(
               *emulator_.GetJob(Param(request, "project"), Param(request, "job")));
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
          std::vector<std::string> dataset_ids = emulator_.ListDatasets(project);
          // Datasets whose names start with an underscore are hidden unless `all` asks for them.
          if (!QueryParamBool(request, "all")) {
            std::erase_if(dataset_ids, [](const std::string& id) { return id.starts_with('_'); });
          }
          return DatasetList(project, dataset_ids, ParseListPage(request));
        }));
    Post("/projects/:project/datasets",
         Json([this](const httplib::Request& request, httplib::Response&) {
           const DatasetReference dataset =
               ParseDatasetInsert(Param(request, "project"), ParseBody(request));
           emulator_.CreateDataset(dataset);
           return DatasetResource(dataset);
         }));
    Get("/projects/:project/datasets/:dataset",
        Json([this](const httplib::Request& request, httplib::Response&) {
          const DatasetReference dataset = DatasetFromPath(request);
          emulator_.GetDataset(dataset);
          return DatasetResource(dataset);
        }));
    // datasets.patch and datasets.update: the emulator keeps no dataset metadata besides the
    // reference, so both only check that the dataset exists.
    const auto update_dataset = Json([this](const httplib::Request& request, httplib::Response&) {
      const DatasetReference dataset = DatasetFromPath(request);
      if (!ParseBody(request).is_object()) throw ApiError::Invalid("Invalid dataset resource");
      emulator_.GetDataset(dataset);
      return DatasetResource(dataset);
    });
    Patch("/projects/:project/datasets/:dataset", update_dataset);
    Put("/projects/:project/datasets/:dataset", update_dataset);
    Delete("/projects/:project/datasets/:dataset",
           Json([this](const httplib::Request& request, httplib::Response& response) {
             emulator_.DeleteDataset(DatasetFromPath(request),
                                     QueryParamBool(request, "deleteContents"));
             response.status = 204;
             return json::object();
           }));

    // tables
    Get("/projects/:project/datasets/:dataset/tables",
        Json([this](const httplib::Request& request, httplib::Response&) {
          const DatasetReference dataset = DatasetFromPath(request);
          return TableList(dataset, emulator_.ListTableEntries(dataset), ParseListPage(request));
        }));
    Post("/projects/:project/datasets/:dataset/tables",
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
    Get("/projects/:project/datasets/:dataset/tables/:table",
        Json([this](const httplib::Request& request, httplib::Response&) {
          return TableResource(emulator_.GetTable(TableFromPath(request)));
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
    Patch("/projects/:project/datasets/:dataset/tables/:table", update_table(/*patch=*/true));
    Put("/projects/:project/datasets/:dataset/tables/:table", update_table(/*patch=*/false));
    Delete("/projects/:project/datasets/:dataset/tables/:table",
           Json([this](const httplib::Request& request, httplib::Response& response) {
             emulator_.DeleteTable(TableFromPath(request));
             response.status = 204;
             return json::object();
           }));
    Get("/projects/:project/datasets/:dataset/tables/:table/data",
        Json([this](const httplib::Request& request, httplib::Response&) {
          const TableReference table = TableFromPath(request);
          const ResultPage page = ParseResultPage(request);
          const QueryResult result =
              emulator_.ListTableData(table, page.start_index, page.max_results);
          return TableDataList(result, emulator_.GetTable(table).num_rows, page);
        }));
    Post("/projects/:project/datasets/:dataset/tables/:table/insertAll",
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
      std::lock_guard<std::mutex> lock(uploads_mutex_);
      id = std::to_string(next_upload_id_++);
      uploads_.emplace(id, std::move(request));
    }
    response.set_header("Location",
                        server_.root_url() + std::string(kResumablePath) + project + "/jobs/" + id);
    return json::object();
  }

  // Closes the resumable upload session `id` and returns the load job it was opened for.
  LoadRequest TakeResumableUpload(const std::string& id) {
    std::lock_guard<std::mutex> lock(uploads_mutex_);
    const auto it = uploads_.find(id);
    if (it == uploads_.end()) throw ApiError::NotFound("Upload session not found");
    LoadRequest request = std::move(it->second);
    uploads_.erase(it);
    return request;
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
