#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "nlohmann/json_fwd.hpp"

namespace bigquery_emulator_duckdb {
class ApiError;
struct Job;
struct Project;
struct DatasetReference;
struct DatasetMetadata;
struct DatasetListEntry;
struct TableInfo;
struct TableListEntry;
struct QueryResult;
struct InsertError;
}  // namespace bigquery_emulator_duckdb

namespace bigquery_emulator_duckdb::server {

struct JobListRequest;
struct ResultPage;
struct ListPage;
struct TableGetRequest;

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
