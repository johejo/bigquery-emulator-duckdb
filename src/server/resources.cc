#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "nlohmann/json.hpp"
#include "src/api_error.h"
#include "src/emulator.h"
#include "src/field_schema.h"
#include "src/references.h"
#include "src/server/internal.h"
#include "src/table_metadata.h"

namespace bigquery_emulator_duckdb::server {
namespace {

using nlohmann::json;

// The emulator does not version resources, so every resource and list has the same etag.
constexpr char kEtag[] = "";

json ErrorProto(const ApiError& error) {
  return json{{"reason", error.reason()}, {"location", "query"}, {"message", error.what()}};
}

json TableReferenceJson(const TableReference& table) {
  return json{{"projectId", table.project_id},
              {"datasetId", table.dataset_id},
              {"tableId", table.table_id}};
}

json DatasetReferenceJson(const DatasetReference& dataset) {
  return json{{"projectId", dataset.project_id}, {"datasetId", dataset.dataset_id}};
}

// The entries of `items`, sorted by `id`, that `page` asks for. Sets nextPageToken on `response`
// when more entries follow.
template <typename T, typename Id>
std::span<const T> ListPageItems(const std::vector<T>& items, const ListPage& page, Id id,
                                 json& response) {
  const auto begin = page.page_token.empty() ? items.begin()
                                             : std::ranges::upper_bound(items, page.page_token,
                                                                        std::ranges::less{}, id);
  const auto end = begin + std::min<int64_t>(items.end() - begin, page.max_results);
  if (end != items.end()) {
    response["nextPageToken"] = std::invoke(id, *(end - 1));
  }
  return {begin, end};
}

std::string TableId(const TableReference& table) {
  return table.project_id + ":" + table.dataset_id + "." + table.table_id;
}

const char* TableTypeName(TableType type) {
  switch (type) {
    case TableType::kTable:
      return "TABLE";
    case TableType::kView:
      return "VIEW";
  }
  return "TABLE";
}

json JobReference(const Job& job) {
  return json{{"projectId", job.project_id}, {"jobId", job.job_id}, {"location", job.location}};
}

std::string JobId(const Job& job) { return job.project_id + ":" + job.location + "." + job.job_id; }

json JobStatus(const Job& job) {
  json status = {{"state", "DONE"}};
  if (job.error.has_value()) {
    status["errorResult"] = ErrorProto(*job.error);
    status["errors"] = json::array({ErrorProto(*job.error)});
  }
  return status;
}

json JobStatistics(const Job& job) {
  json statistics = {{"creationTime", std::to_string(job.creation_time_ms)},
                     {"startTime", std::to_string(job.creation_time_ms)},
                     {"endTime", std::to_string(job.end_time_ms)}};
  if (std::holds_alternative<CopyJob>(job.configuration)) {
    statistics["copy"] = {{"copiedRows", std::to_string(job.output_rows)}};
    return statistics;
  }
  if (std::holds_alternative<LoadJob>(job.configuration)) {
    statistics["load"] = {{"outputRows", std::to_string(job.output_rows)}};
    return statistics;
  }
  if (const auto* extract = std::get_if<ExtractJob>(&job.configuration)) {
    // A finished extract writes one file per destination URI.
    json counts = json::array();
    if (!job.error.has_value()) {
      for (size_t i = 0; i < extract->destination_uris.size(); ++i) counts.push_back("1");
    }
    statistics["extract"] = {{"destinationUriFileCounts", std::move(counts)}};
    return statistics;
  }
  const QueryJob& query = *job.query();
  json query_statistics = {
      {"totalBytesProcessed", "0"}, {"totalBytesBilled", "0"}, {"cacheHit", false}};
  // A query that failed before it was translated has no statement to describe.
  if (!query.statement_type.empty()) {
    query_statistics["statementType"] = query.statement_type;
  }
  if (query.ddl_target_table.has_value()) {
    query_statistics["ddlTargetTable"] = TableReferenceJson(*query.ddl_target_table);
  }
  if (query.ddl_target_dataset.has_value()) {
    query_statistics["ddlTargetDataset"] = DatasetReferenceJson(*query.ddl_target_dataset);
  }
  if (job.result.has_value() && job.result->affected_rows >= 0) {
    query_statistics["numDmlAffectedRows"] = std::to_string(job.result->affected_rows);
  }
  // A dry run reports what the query would return; that schema is all it produces.
  if (job.dry_run() && job.result.has_value() && job.result->has_rows) {
    query_statistics["schema"] = job.result->SchemaToJson();
  }
  statistics["totalBytesProcessed"] = "0";
  statistics["query"] = std::move(query_statistics);
  return statistics;
}

// The configuration a job resource reports, with the defaults the request left out filled in.
json JobConfiguration(const Job& job) {
  const auto complete = [&job](TableReference table) {
    if (table.project_id.empty()) table.project_id = job.project_id;
    return TableReferenceJson(table);
  };
  if (const auto* copy = std::get_if<CopyJob>(&job.configuration)) {
    json config = copy->configuration;
    config["destinationTable"] = complete(copy->destination_table);
    if (config.contains("sourceTable")) {
      config["sourceTable"] = complete(copy->source_tables.at(0));
    } else {
      config["sourceTables"] = json::array();
      for (const TableReference& source : copy->source_tables) {
        config["sourceTables"].push_back(complete(source));
      }
    }
    config["createDisposition"] = DispositionName(copy->create_disposition);
    config["writeDisposition"] = DispositionName(copy->write_disposition);
    return json{{"jobType", "COPY"}, {"copy", std::move(config)}};
  }
  if (const auto* load = std::get_if<LoadJob>(&job.configuration)) {
    json config = load->configuration;
    config["destinationTable"] = complete(load->destination_table);
    return json{{"jobType", "LOAD"}, {"load", std::move(config)}};
  }
  if (const auto* extract = std::get_if<ExtractJob>(&job.configuration)) {
    json config = extract->configuration;
    config["sourceTable"] = complete(extract->source_table);
    return json{{"jobType", "EXTRACT"}, {"extract", std::move(config)}};
  }
  const QueryJob& query = *job.query();
  json config{{"query", query.query}, {"useLegacySql", false}};
  if (query.destination_table.has_value()) {
    config["destinationTable"] = complete(*query.destination_table);
    config["createDisposition"] = DispositionName(query.create_disposition);
    config["writeDisposition"] = DispositionName(query.write_disposition);
  }
  return json{{"jobType", "QUERY"}, {"dryRun", query.dry_run}, {"query", std::move(config)}};
}

json JobListEntry(const Job& job, bool full) {
  json entry = {{"kind", "bigquery#job"},
                {"id", JobId(job)},
                {"jobReference", JobReference(job)},
                {"state", "DONE"},
                {"configuration", JobConfiguration(job)},
                {"statistics", JobStatistics(job)}};
  if (job.error.has_value()) {
    entry["errorResult"] = ErrorProto(*job.error);
  }
  if (full) {
    entry["status"] = JobStatus(job);
  }
  return entry;
}

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

// A dry run creates no job, so its response has no job reference either.
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

}  // namespace

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

json JobResource(const Job& job) {
  return json{{"kind", "bigquery#job"},
              {"etag", kEtag},
              {"id", JobId(job)},
              {"selfLink", ""},
              {"jobReference", JobReference(job)},
              {"configuration", JobConfiguration(job)},
              {"status", JobStatus(job)},
              {"statistics", JobStatistics(job)}};
}

json JobList(const std::vector<std::shared_ptr<const Job>>& jobs, const JobListRequest& request) {
  json response = {{"kind", "bigquery#jobList"}, {"etag", kEtag}};
  json entries = json::array();
  int64_t index = 0;
  // Every job is done and none has a parent, so those filters match nothing.
  const bool none =
      request.parent_filter || (!request.state_filter.empty() && request.state_filter != "done");
  for (const auto& job : jobs) {
    if (none || job->creation_time_ms < request.min_creation_time ||
        job->creation_time_ms > request.max_creation_time) {
      continue;
    }
    if (index++ < request.offset) {
      continue;
    }
    if (static_cast<int64_t>(entries.size()) == request.max_results) {
      response["nextPageToken"] = std::to_string(index - 1);
      break;
    }
    entries.push_back(JobListEntry(*job, request.full_projection));
  }
  if (!entries.empty()) {
    response["jobs"] = std::move(entries);
  }
  return response;
}

json JobCancelResponse(const Job& job) {
  return json{{"kind", "bigquery#jobCancelResponse"}, {"job", JobResource(job)}};
}

json QueryResponse(const Job& job, const ResultPage& page) {
  if (job.dry_run()) {
    return DryRunQueryResponse(job);
  }
  json response = {{"kind", "bigquery#queryResponse"}};
  AddQueryResults(job, page, response);
  return response;
}

json GetQueryResultsResponse(const Job& job, const ResultPage& page) {
  json response = {{"kind", "bigquery#getQueryResultsResponse"}, {"etag", kEtag}};
  AddQueryResults(job, page, response);
  return response;
}

json DatasetResource(const DatasetReference& dataset) {
  return json{{"kind", "bigquery#dataset"},
              {"etag", kEtag},
              {"id", dataset.project_id + ":" + dataset.dataset_id},
              {"datasetReference", DatasetReferenceJson(dataset)},
              {"location", kLocation}};
}

json DatasetList(const std::string& project_id, const std::vector<std::string>& dataset_ids,
                 const ListPage& page) {
  json response = {{"kind", "bigquery#datasetList"}, {"etag", kEtag}};
  json datasets = json::array();
  for (const std::string& dataset_id :
       ListPageItems(dataset_ids, page, std::identity{}, response)) {
    datasets.push_back(DatasetResource(DatasetReference{project_id, dataset_id}));
  }
  // BigQuery omits the datasets of a project that has none.
  if (!datasets.empty()) {
    response["datasets"] = std::move(datasets);
  }
  return response;
}

json TableResource(const TableInfo& info) {
  json resource{{"kind", "bigquery#table"},
                {"etag", kEtag},
                {"id", TableId(info.reference)},
                {"tableReference", TableReferenceJson(info.reference)},
                {"schema", SchemaToJson(info.schema)},
                {"type", TableTypeName(info.view_query ? TableType::kView : TableType::kTable)},
                {"numRows", std::to_string(info.num_rows)},
                {"numBytes", "0"},
                {"location", kLocation}};
  resource.update(info.metadata.ToJson());
  if (info.view_query) {
    resource["view"] = {{"query", *info.view_query}, {"useLegacySql", false}};
    resource.erase("numRows");
    resource.erase("numBytes");
  }
  return resource;
}

json TableList(const DatasetReference& dataset, const std::vector<TableListEntry>& tables,
               const ListPage& page) {
  json response = {{"kind", "bigquery#tableList"}, {"etag", kEtag}, {"totalItems", tables.size()}};
  json entries = json::array();
  for (const TableListEntry& entry :
       ListPageItems(tables, page, &TableListEntry::table_id, response)) {
    const TableReference table{dataset.project_id, dataset.dataset_id, entry.table_id};
    json item{{"kind", "bigquery#table"},
              {"id", TableId(table)},
              {"tableReference", TableReferenceJson(table)},
              {"type", TableTypeName(entry.type)}};
    // A list entry carries the friendly name and labels but not the description.
    if (!entry.metadata.friendly_name.empty()) {
      item["friendlyName"] = entry.metadata.friendly_name;
    }
    if (!entry.metadata.labels.empty()) {
      item["labels"] = entry.metadata.labels;
    }
    entries.push_back(std::move(item));
  }
  response["tables"] = std::move(entries);
  return response;
}

json TableDataList(const QueryResult& result, int64_t total_rows, const ResultPage& page) {
  const auto size = static_cast<int64_t>(result.rows.size());
  json response = {{"kind", "bigquery#tableDataList"},
                   {"etag", kEtag},
                   {"totalRows", std::to_string(total_rows)},
                   {"rows", RowsForResponse(result, 0, size, page.int64_timestamps)}};
  if (page.start_index + size < total_rows) {
    response["pageToken"] = std::to_string(page.start_index + size);
  }
  return response;
}

json InsertAllResponse(const std::vector<InsertError>& errors) {
  json response = {{"kind", "bigquery#tableDataInsertAllResponse"}};
  if (!errors.empty()) {
    response["insertErrors"] = json::array();
    for (const InsertError& error : errors) {
      response["insertErrors"].push_back(
          {{"index", error.index},
           {"errors", json::array({{{"reason", "invalid"}, {"message", error.message}}})}});
    }
  }
  return response;
}

}  // namespace bigquery_emulator_duckdb::server
