#pragma once

// Shared by the server's sources; not part of its interface, which is src/server.h.

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "httplib.h"
#include "nlohmann/json.hpp"
#include "src/api_error.h"
#include "src/emulator.h"
#include "src/field_schema.h"
#include "src/project.h"
#include "src/references.h"
#include "src/table_metadata.h"

namespace bigquery_emulator_duckdb::server {

// routes.cc: the methods of the discovery document, which the server routes requests by.

// A query parameter of a method, as the discovery document describes it.
struct QueryParameter {
  std::string type;    // "string", "integer" or "boolean"
  std::string format;  // such as "int32", "uint32" or "uint64"; empty for any value of the type
  std::vector<std::string> values;  // the values an enum takes; empty for any value
};

struct ApiMethod {
  std::string http_method;
  // The cpp-httplib patterns of the method's path below the API prefix, such as
  // "/projects/:projectId/jobs", and of its media upload paths below the root.
  std::string path;
  std::vector<std::string> upload_paths;
  // The query parameters it takes, the API-wide ones such as prettyPrint included.
  std::map<std::string, QueryParameter, std::less<>> parameters;
};

// The parsed discovery document.
const nlohmann::json& Discovery();
// The discovery document's method `id`, such as "bigquery.tables.get". Throws std::logic_error
// when the document lacks it or cpp-httplib cannot match its path.
ApiMethod FindApiMethod(std::string_view id);
// Rejects a request with a query parameter `method` does not take or a value its type does not
// allow, as BigQuery does, and with a non-empty one the emulator does not handle as unsupported.
// The handler of `method` handles the parameters `accepted`, reading or deliberately ignoring
// them; every method handles some of the API-wide ones.
void CheckQueryParameters(const ApiMethod& method, const std::vector<std::string>& accepted,
                          const httplib::Request& request);

// requests.cc: HTTP requests to the emulator's request structs. Every malformed request is
// rejected with ApiError::Invalid.

// The page size of query results and table data when the request leaves it out.
constexpr int64_t kDefaultMaxResults = 100000;

// The slice of a job's or table's rows that one response carries.
struct ResultPage {
  int64_t start_index = 0;
  int64_t max_results = kDefaultMaxResults;
  // Clients that ask for formatOptions.useInt64Timestamp get TIMESTAMP values as epoch
  // microseconds instead of the default decimal seconds.
  bool int64_timestamps = false;
};

struct JobListRequest {
  bool full_projection = true;
  std::string state_filter;
  std::optional<std::string> parent_job_id;
  int64_t max_results = 50;
  int64_t offset = 0;
  int64_t min_creation_time = 0;
  int64_t max_creation_time = INT64_MAX;
};

// A page of projects.list, datasets.list or tables.list, ordered by ID. The token is the ID of the
// last entry of the previous page; pages stay consistent when entries are added or removed.
struct ListPage {
  int64_t max_results = INT64_MAX;
  std::string page_token;
};

// A missing value matches any value of the label, including the empty string.
using DatasetFilter = std::map<std::string, std::optional<std::string>>;

struct TableGetRequest {
  bool storage_stats = true;
  std::string selected_fields;
};

struct DatasetInsertRequest {
  DatasetReference dataset;
  DatasetMetadata metadata;
};

struct TableInsertRequest {
  TableReference table;
  // The view definition, when the request creates a view rather than a table.
  std::optional<nlohmann::json> view;
  std::vector<FieldSchema> schema;
  TableMetadata metadata;
};

// What tables.patch and tables.update change. The emulator keeps what they leave out.
struct TableUpdateRequest {
  std::optional<std::vector<FieldSchema>> schema;
  std::optional<nlohmann::json> view;
};

struct InsertAllRequest {
  nlohmann::json rows;
  bool skip_invalid_rows = false;
  bool ignore_unknown_values = false;
};

// A multipart upload: the job resource and the bytes to load.
struct MediaUpload {
  nlohmann::json metadata;
  std::string content;
};

using JobRequest = std::variant<QueryRequest, LoadRequest, CopyRequest, ExtractRequest>;

std::string Param(const httplib::Request& request, const char* name);
bool QueryParamBool(const httplib::Request& request, const char* name);
DatasetReference DatasetFromPath(const httplib::Request& request);
TableReference TableFromPath(const httplib::Request& request);

nlohmann::json ParseBody(const httplib::Request& request);
bool IsResumableUpload(const httplib::Request& request);
bool IsMultipartUpload(const httplib::Request& request);
MediaUpload ParseMultipartUpload(const httplib::Request& request);

// jobs.query, whose body is the query configuration and its page.
QueryRequest ParseQuery(const std::string& project_id, const nlohmann::json& body);
ResultPage ParseQueryPage(const nlohmann::json& body);
// jobs.getQueryResults and tabledata.list, whose page is in the query string.
ResultPage ParseResultPage(const httplib::Request& request);
// jobs.insert with a job resource of any supported type.
JobRequest ParseJobInsert(const std::string& project_id, const nlohmann::json& body);
// jobs.insert with a job resource that has to be a load job, as media uploads do.
LoadRequest ParseLoadInsert(const std::string& project_id, const nlohmann::json& body);
JobListRequest ParseJobList(const httplib::Request& request);
// datasets.list and tables.list.
ListPage ParseListPage(const httplib::Request& request);
DatasetFilter ParseDatasetFilter(const httplib::Request& request);
TableGetRequest ParseTableGet(const httplib::Request& request);
DatasetInsertRequest ParseDatasetInsert(const std::string& project_id, const nlohmann::json& body);
TableInsertRequest ParseTableInsert(const DatasetReference& dataset, const nlohmann::json& body);
TableUpdateRequest ParseTableUpdate(const nlohmann::json& body);
InsertAllRequest ParseInsertAll(const nlohmann::json& body);

// resources.cc: the emulator's structs to BigQuery's JSON resources and responses.

nlohmann::json ErrorBody(const ApiError& error);
nlohmann::json JobResource(const Job& job);
// Filters and pages `jobs` as `request` asks.
nlohmann::json JobList(const std::vector<std::shared_ptr<const Job>>& jobs,
                       const JobListRequest& request);
nlohmann::json JobCancelResponse(const Job& job);
nlohmann::json QueryResponse(const Job& job, const ResultPage& page);
nlohmann::json GetQueryResultsResponse(const Job& job, const ResultPage& page);
nlohmann::json ProjectList(const std::vector<Project>& projects, const ListPage& page);

nlohmann::json DatasetResource(const DatasetReference& dataset, const DatasetMetadata& metadata);
// `entries` and `tables` are sorted by id; the lists carry the page of them `page` asks for.
nlohmann::json DatasetList(const std::string& project_id,
                           const std::vector<DatasetListEntry>& entries, const ListPage& page);
nlohmann::json TableResource(const TableInfo& info);
// Applies tables.get's schema selection and metadata view to the resource.
nlohmann::json TableGetResource(const TableInfo& info, const TableGetRequest& request);
nlohmann::json TableList(const DatasetReference& dataset, const std::vector<TableListEntry>& tables,
                         const ListPage& page);
// `result` holds the rows from `page.start_index` on; the table has `total_rows`.
nlohmann::json TableDataList(const QueryResult& result, int64_t total_rows, const ResultPage& page,
                             std::string_view selected_fields);
nlohmann::json InsertAllResponse(const std::vector<InsertError>& errors);

}  // namespace bigquery_emulator_duckdb::server
