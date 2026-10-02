#pragma once

// Shared by the server's sources; not part of its interface, which is src/server.h.

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "httplib.h"
#include "nlohmann/json.hpp"
#include "src/api_error.h"
#include "src/emulator.h"
#include "src/field_schema.h"
#include "src/references.h"
#include "src/table_metadata.h"

namespace bigquery_emulator_duckdb::server {

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
  bool parent_filter = false;
  int64_t max_results = 50;
  int64_t offset = 0;
  int64_t min_creation_time = 0;
  int64_t max_creation_time = INT64_MAX;
};

// A page of datasets.list or tables.list. Both list by id, so the page token is the id of the
// last entry of the previous page; pages stay consistent when entries are added or removed.
struct ListPage {
  int64_t max_results = INT64_MAX;
  std::string page_token;
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
// Rejects a non-empty query parameter `name` that would change the response but is not emulated.
void RejectQueryParam(const httplib::Request& request, const char* name);
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
DatasetReference ParseDatasetInsert(const std::string& project_id, const nlohmann::json& body);
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
nlohmann::json DatasetResource(const DatasetReference& dataset);
// `dataset_ids` and `tables` are sorted by id; the lists carry the page of them `page` asks for.
nlohmann::json DatasetList(const std::string& project_id,
                           const std::vector<std::string>& dataset_ids, const ListPage& page);
nlohmann::json TableResource(const TableInfo& info);
nlohmann::json TableList(const DatasetReference& dataset, const std::vector<TableListEntry>& tables,
                         const ListPage& page);
// `result` holds the rows from `page.start_index` on; the table has `total_rows`.
nlohmann::json TableDataList(const QueryResult& result, int64_t total_rows, const ResultPage& page);
nlohmann::json InsertAllResponse(const std::vector<InsertError>& errors);

}  // namespace bigquery_emulator_duckdb::server
