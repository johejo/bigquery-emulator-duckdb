#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "nlohmann/json.hpp"
#include "src/emulator.h"
#include "src/field_schema.h"
#include "src/references.h"
#include "src/table_metadata.h"

namespace httplib {
struct Request;
}

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
  // The states asked for; empty asks for every state.
  std::vector<std::string> state_filters;
  std::optional<std::string> parent_job_id;
  int64_t max_results = 50;
  int64_t offset = 0;
  int64_t min_creation_time = 0;
  int64_t max_creation_time = INT64_MAX;
};

// A page of resource listings, ordered by ID. The token is the ID of the
// last entry of the previous page; pages stay consistent when entries are added or removed.
struct ListPage {
  int64_t max_results = INT64_MAX;
  std::string page_token;
};

// A missing value matches any value of the label, including the empty string.
using DatasetFilter = std::map<std::string, std::optional<std::string>>;

// A JSON field mask, with whole fields taking precedence over their children.
struct ResourceMask {
  bool all = false;
  std::map<std::string, ResourceMask> fields;
};

struct RoutineListRequest {
  ListPage page;
  std::string routine_type;
  ResourceMask read_mask;
};

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
RoutineReference RoutineFromPath(const httplib::Request& request);

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
// Resource listings ordered by ID.
ListPage ParseListPage(const httplib::Request& request);
DatasetFilter ParseDatasetFilter(const httplib::Request& request);
RoutineListRequest ParseRoutineList(const httplib::Request& request);
TableGetRequest ParseTableGet(const httplib::Request& request);
DatasetInsertRequest ParseDatasetInsert(const std::string& project_id, const nlohmann::json& body);
TableInsertRequest ParseTableInsert(const DatasetReference& dataset, const nlohmann::json& body);
TableUpdateRequest ParseTableUpdate(const nlohmann::json& body);
InsertAllRequest ParseInsertAll(const nlohmann::json& body);

}  // namespace bigquery_emulator_duckdb::server
