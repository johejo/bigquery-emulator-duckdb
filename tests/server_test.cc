#include "src/server.h"

#include <memory>
#include <string>
#include <thread>

#include "gtest/gtest.h"
#include "httplib.h"
#include "nlohmann/json.hpp"
#include "src/emulator.h"

namespace bigquery_emulator_duckdb {
namespace {

using nlohmann::json;

// Verify REST status codes and response fields here; client behavior is covered in tests/e2e.
class ServerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ServerOptions options;
    options.host = "127.0.0.1";
    options.port = 0;
    server_ = std::make_unique<Server>(emulator_, options);
    ASSERT_TRUE(server_->Bind());
    thread_ = std::thread([this] { server_->Serve(); });
    client_ = std::make_unique<httplib::Client>(server_->root_url());
  }

  void TearDown() override {
    server_->Stop();
    thread_.join();
  }

  json Get(const std::string& path, int expected_status = 200) {
    const httplib::Result result = client_->Get(path);
    EXPECT_TRUE(result) << "request failed";
    EXPECT_EQ(result->status, expected_status) << result->body;
    return json::parse(result->body);
  }

  json Post(const std::string& path, const json& body, int expected_status = 200) {
    const httplib::Result result = client_->Post(path, body.dump(), "application/json");
    EXPECT_TRUE(result) << "request failed";
    EXPECT_EQ(result->status, expected_status) << result->body;
    return json::parse(result->body);
  }

  Emulator emulator_;
  std::unique_ptr<Server> server_;
  std::thread thread_;
  std::unique_ptr<httplib::Client> client_;
};

TEST_F(ServerTest, ServesDiscoveryDocument) {
  const json document = Get("/$discovery/rest?version=v2");
  EXPECT_EQ(document["rootUrl"], server_->root_url() + "/");
  EXPECT_EQ(document["baseUrl"], server_->root_url() + "/bigquery/v2/");
  EXPECT_TRUE(document["resources"].contains("jobs"));
}

TEST_F(ServerTest, ReportsQueryErrors) {
  const json response = Post("/bigquery/v2/projects/p/queries", {{"query", "SELECT * FROM nope"}});
  ASSERT_TRUE(response.contains("errors"));
  EXPECT_EQ(response["errors"][0]["reason"], "invalidQuery");
}

TEST_F(ServerTest, RejectsLegacySql) {
  const json query =
      Post("/bigquery/v2/projects/p/queries", {{"query", "SELECT 1"}, {"useLegacySql", true}}, 400);
  EXPECT_EQ(query["error"]["errors"][0]["reason"], "invalid");
  const json job =
      Post("/bigquery/v2/projects/p/jobs",
           {{"configuration", {{"query", {{"query", "SELECT 1"}, {"useLegacySql", true}}}}}}, 400);
  EXPECT_EQ(job["error"]["errors"][0]["reason"], "invalid");
  Post("/bigquery/v2/projects/p/queries", {{"query", "SELECT 1"}, {"useLegacySql", false}});
}

TEST_F(ServerTest, ListsJobsWithFiltersAndProjection) {
  const std::string path = "/bigquery/v2/projects/p/jobs";
  Post(path, {{"jobReference", {{"jobId", "a"}}},
              {"configuration", {{"query", {{"query", "SELECT 1"}}}}}});
  Post(path, {{"jobReference", {{"jobId", "b"}}},
              {"configuration", {{"query", {{"query", "SELECT * FROM missing"}}}}}});
  Post("/projects/other/jobs", {{"jobReference", {{"jobId", "c"}}},
                                {"configuration", {{"query", {{"query", "SELECT 1"}}}}}});
  const json minimal = Get(path + "?projection=minimal");
  EXPECT_EQ(minimal["kind"], "bigquery#jobList");
  ASSERT_EQ(minimal["jobs"].size(), 2);
  EXPECT_FALSE(minimal["jobs"][0].contains("status"));
  const json full = Get(path + "?pageToken=");
  ASSERT_EQ(full["jobs"].size(), 2);
  EXPECT_TRUE(full["jobs"][0].contains("status"));
  EXPECT_FALSE(Get(path + "?stateFilter=running").contains("jobs"));
  EXPECT_FALSE(Get(path + "?parentJobId=parent").contains("jobs"));
  Get(path + "?maxResults=-1", 400);
  Get(path + "?projection=unknown", 400);
}

TEST_F(ServerTest, CancelsCompletedJobWithoutChangingIt) {
  const std::string path = "/projects/p/jobs";
  const json job = Post(path, {{"jobReference", {{"jobId", "done"}}},
                               {"configuration", {{"query", {{"query", "SELECT 1"}}}}}});
  const json cancelled = Post(path + "/done/cancel", json::object());
  EXPECT_EQ(cancelled["kind"], "bigquery#jobCancelResponse");
  EXPECT_EQ(cancelled["job"], job);
  EXPECT_EQ(Get(path + "/done"), job);
  Post(path + "/missing/cancel", json::object(), 404);
}

TEST_F(ServerTest, DeletesJobMetadataButKeepsDestinationTable) {
  const std::string path = "/bigquery/v2/projects/p/jobs";
  Post("/projects/p/datasets", {{"datasetReference", {{"datasetId", "ds"}}}});
  const json request = {{"jobReference", {{"jobId", "old"}}},
                        {"configuration",
                         {{"query",
                           {{"query", "SELECT 1 AS x"},
                            {"destinationTable", {{"datasetId", "ds"}, {"tableId", "result"}}}}}}}};
  Post(path, request);
  EXPECT_EQ(Get("/projects/p/queries/old")["kind"], "bigquery#getQueryResultsResponse");

  const httplib::Result deleted = client_->Delete(path + "/old/delete?location=US");
  ASSERT_TRUE(deleted);
  EXPECT_EQ(deleted->status, 204) << deleted->body;
  Get(path + "/old", 404);
  Get("/projects/p/queries/old", 404);
  EXPECT_FALSE(Get(path).contains("jobs"));
  EXPECT_EQ(Get("/projects/p/datasets/ds/tables/result/data")["rows"][0]["f"][0]["v"], "1");
  EXPECT_EQ(client_->Delete(path + "/old/delete")->status, 404);
}

TEST_F(ServerTest, RejectsDuplicateJobIdWithoutRunningQuery) {
  const std::string path = "/bigquery/v2/projects/p/jobs";
  const json first = {{"jobReference", {{"jobId", "same"}}},
                      {"configuration", {{"query", {{"query", "SELECT 1 AS x"}}}}}};
  Post(path, first);

  const json duplicate = {{"jobReference", {{"jobId", "same"}}},
                          {"configuration", {{"query", {{"query", "SELECT 2 AS x"}}}}}};
  const json error = Post(path, duplicate, 409);
  EXPECT_EQ(error["error"]["errors"][0]["reason"], "duplicate");
  EXPECT_EQ(error["error"]["status"], "ALREADY_EXISTS");
  EXPECT_EQ(Get("/bigquery/v2/projects/p/queries/same")["rows"][0]["f"][0]["v"], "1");
}

TEST_F(ServerTest, ReportsDefaultQueryJobConfiguration) {
  Post("/bigquery/v2/projects/p/datasets", {{"datasetReference", {{"datasetId", "ds"}}}});
  const json job = Post("/bigquery/v2/projects/p/jobs",
                        {{"configuration",
                          {{"query",
                            {{"query", "SELECT 1 AS x"},
                             {"destinationTable", {{"datasetId", "ds"}, {"tableId", "t"}}}}}}}});
  EXPECT_EQ(job["statistics"]["query"]["statementType"], "SELECT");
  const json& query = job["configuration"]["query"];
  EXPECT_EQ(query["destinationTable"]["projectId"], "p");
  EXPECT_EQ(query["createDisposition"], "CREATE_IF_NEEDED");
  EXPECT_EQ(query["writeDisposition"], "WRITE_EMPTY");
}

TEST_F(ServerTest, RejectsUnknownDispositionsWithoutCreatingJobs) {
  Post("/bigquery/v2/projects/p/datasets", {{"datasetReference", {{"datasetId", "ds"}}}});
  const json error = Post("/bigquery/v2/projects/p/jobs",
                          {{"jobReference", {{"jobId", "bad"}}},
                           {"configuration",
                            {{"query",
                              {{"query", "SELECT 1 AS x"},
                               {"destinationTable", {{"datasetId", "ds"}, {"tableId", "t"}}},
                               {"writeDisposition", "WRITE_SOMETIMES"}}}}}},
                          400);
  EXPECT_EQ(error["error"]["errors"][0]["reason"], "invalid");
  Get("/bigquery/v2/projects/p/jobs/bad", 404);
}

// No client library reads ddlTargetDataset, so it is checked here rather than end to end.
TEST_F(ServerTest, ReportsTheDatasetOfSchemaDdl) {
  const json job = Post("/bigquery/v2/projects/p/jobs",
                        {{"configuration", {{"query", {{"query", "CREATE SCHEMA created"}}}}}});
  EXPECT_EQ(job["statistics"]["query"]["statementType"], "CREATE_SCHEMA");
  EXPECT_EQ(job["statistics"]["query"]["ddlTargetDataset"],
            json::parse(R"({"projectId": "p", "datasetId": "created"})"));
  EXPECT_FALSE(job["statistics"]["query"].contains("ddlTargetTable"));
}

TEST_F(ServerTest, ReportsCopyJobStatisticsAndErrors) {
  const std::string jobs = "/projects/p/jobs";
  Post("/projects/p/datasets", {{"datasetReference", {{"datasetId", "ds"}}}});
  Post("/projects/p/queries", {{"query", "CREATE TABLE ds.source AS SELECT 1 AS id"}});
  const json source = {{"datasetId", "ds"}, {"tableId", "source"}};
  const json destination = {{"datasetId", "ds"}, {"tableId", "target"}};
  const json copied =
      Post(jobs, {{"jobReference", {{"jobId", "copy1"}}},
                  {"configuration",
                   {{"copy", {{"sourceTable", source}, {"destinationTable", destination}}}}}});
  EXPECT_EQ(copied["configuration"]["jobType"], "COPY");
  EXPECT_EQ(copied["configuration"]["copy"]["sourceTable"]["projectId"], "p");
  EXPECT_EQ(copied["statistics"]["copy"]["copiedRows"], "1");
  EXPECT_EQ(Get(jobs + "/copy1"), copied);

  const json failed =
      Post(jobs, {{"configuration",
                   {{"copy", {{"sourceTable", source}, {"destinationTable", destination}}}}}});
  EXPECT_EQ(failed["status"]["errorResult"]["reason"], "duplicate");
  const json appended = Post(jobs, {{"configuration",
                                     {{"copy",
                                       {{"sourceTables", json::array({source, source})},
                                        {"destinationTable", destination},
                                        {"writeDisposition", "WRITE_APPEND"}}}}}});
  EXPECT_EQ(appended["statistics"]["copy"]["copiedRows"], "2");
  const json missing =
      Post(jobs, {{"configuration",
                   {{"copy",
                     {{"sourceTable", source},
                      {"destinationTable", {{"datasetId", "ds"}, {"tableId", "absent"}}},
                      {"createDisposition", "CREATE_NEVER"}}}}}});
  EXPECT_EQ(missing["status"]["errorResult"]["reason"], "notFound");
}

// Every job type reads table references the same way, so a malformed one is a bad request
// rather than an internal error, and the job is never run.
TEST_F(ServerTest, RejectsMalformedTableReferencesInEveryJobType) {
  const std::string jobs = "/projects/p/jobs";
  const json source = {{"datasetId", "ds"}, {"tableId", "source"}};
  for (const json& table :
       {json("ds.t"), json{{"datasetId", "ds"}, {"tableId", 1}}, json{{"datasetId", "ds"}}}) {
    Post(jobs,
         {{"configuration", {{"query", {{"query", "SELECT 1"}, {"destinationTable", table}}}}}},
         400);
    Post(jobs,
         {{"configuration",
           {{"load", {{"sourceUris", json::array({"a.csv"})}, {"destinationTable", table}}}}}},
         400);
    Post(jobs,
         {{"configuration", {{"copy", {{"sourceTable", table}, {"destinationTable", source}}}}}},
         400);
  }
  EXPECT_FALSE(Get(jobs).contains("jobs"));
}

// A resumable upload session is only opened for a load job the emulator can run.
TEST_F(ServerTest, RejectsResumableUploadsOfInvalidLoadJobs) {
  for (const std::string path : {"/resumable/upload/bigquery/v2/projects/p/jobs",
                                 "/upload/bigquery/v2/projects/p/jobs?uploadType=resumable"}) {
    const httplib::Result query =
        client_->Post(path, json{{"configuration", {{"query", {{"query", "SELECT 1"}}}}}}.dump(),
                      "application/json");
    ASSERT_TRUE(query);
    EXPECT_EQ(query->status, 400) << query->body;
    const httplib::Result load = client_->Post(
        path, json{{"configuration", {{"load", {{"destinationTable", "ds.t"}}}}}}.dump(),
        "application/json");
    ASSERT_TRUE(load);
    EXPECT_EQ(load->status, 400) << load->body;
    EXPECT_FALSE(load->has_header("Location"));
  }
}

TEST_F(ServerTest, ReportsDryRunResponsesWithoutCreatingJobs) {
  Post("/bigquery/v2/projects/p/datasets", {{"datasetReference", {{"datasetId", "ds"}}}});
  Post("/bigquery/v2/projects/p/queries", {{"query", "CREATE TABLE ds.t (id INT64)"}});

  const json response = Post("/bigquery/v2/projects/p/queries",
                             {{"query", "SELECT id FROM `p.ds.t`"}, {"dryRun", true}});
  EXPECT_EQ(response["kind"], "bigquery#queryResponse");
  EXPECT_EQ(response["jobComplete"], true);
  EXPECT_EQ(response["schema"]["fields"][0]["name"], "id");
  EXPECT_FALSE(response.contains("jobReference"));
  EXPECT_FALSE(response.contains("rows"));

  // The statement is validated but never runs, and no job is created for it.
  const json job = Post("/bigquery/v2/projects/p/jobs", {{"jobReference", {{"jobId", "dry1"}}},
                                                         {"configuration",
                                                          {{"dryRun", true},
                                                           {"query",
                                                            {{"query",
                                                              "INSERT INTO ds.t VALUES "
                                                              "(1)"}}}}}});
  EXPECT_EQ(job["configuration"]["dryRun"], true);
  EXPECT_FALSE(job["status"].contains("errorResult"));
  Get("/bigquery/v2/projects/p/jobs/dry1", 404);

  Post("/bigquery/v2/projects/p/queries", {{"query", "SELECT * FROM missing"}, {"dryRun", true}},
       400);
}

TEST_F(ServerTest, EncodesTimestampsAsMicrosecondsOnRequest) {
  const json body = {{"query", "SELECT TIMESTAMP '2020-01-02 03:04:05+00' AS ts"}};
  EXPECT_EQ(Post("/bigquery/v2/projects/p/queries", body)["rows"][0]["f"][0]["v"], "1577934245");

  json with_option = body;
  with_option["formatOptions"] = {{"useInt64Timestamp", true}};
  EXPECT_EQ(Post("/bigquery/v2/projects/p/queries", with_option)["rows"][0]["f"][0]["v"],
            "1577934245000000");

  Post("/bigquery/v2/projects/p/jobs",
       {{"jobReference", {{"jobId", "ts1"}}}, {"configuration", {{"query", body}}}});
  EXPECT_EQ(Get("/bigquery/v2/projects/p/queries/ts1"
                "?formatOptions.useInt64Timestamp=true")["rows"][0]["f"][0]["v"],
            "1577934245000000");
}

TEST_F(ServerTest, ReportsDatasetAndTableResourcesAndErrors) {
  Post("/bigquery/v2/projects/p/datasets", {{"datasetReference", {{"datasetId", "ds"}}}});
  const json duplicate =
      Post("/bigquery/v2/projects/p/datasets", {{"datasetReference", {{"datasetId", "ds"}}}}, 409);
  EXPECT_EQ(duplicate["error"]["status"], "ALREADY_EXISTS");
  EXPECT_EQ(Get("/bigquery/v2/projects/p/datasets")["datasets"][0]["id"], "p:ds");
  Get("/bigquery/v2/projects/p/datasets/missing", 404);

  const json table = Post(
      "/bigquery/v2/projects/p/datasets/ds/tables",
      {{"tableReference", {{"tableId", "t"}}},
       {"schema", {{"fields", json::parse(R"([{"name": "id", "type": "INTEGER", "mode": "REQUIRED"},
                                    {"name": "tags", "type": "STRING", "mode": "REPEATED"}])")}}}});
  EXPECT_EQ(table["id"], "p:ds.t");
  EXPECT_EQ(table["schema"]["fields"][1]["mode"], "REPEATED");
  EXPECT_EQ(Get("/bigquery/v2/projects/p/datasets/ds/tables")["tables"][0]["id"], "p:ds.t");

  EXPECT_EQ(client_->Delete("/bigquery/v2/projects/p/datasets/ds")->status, 400);
  EXPECT_EQ(client_->Delete("/bigquery/v2/projects/p/datasets/ds/tables/t")->status, 204);
  EXPECT_EQ(client_->Delete("/bigquery/v2/projects/p/datasets/ds")->status, 204);
}

TEST_F(ServerTest, EncodesStreamedRows) {
  const std::string base = "/projects/p/datasets/ds/tables/t";
  Post("/projects/p/datasets", {{"datasetReference", {{"datasetId", "ds"}}}});
  Post("/projects/p/datasets/ds/tables",
       {{"tableReference", {{"tableId", "t"}}}, {"schema", {{"fields", json::parse(R"([
          {"name":"id","type":"INTEGER","mode":"REQUIRED"},
          {"name":"tags","type":"STRING","mode":"REPEATED"},
          {"name":"profile","type":"RECORD","fields":[
            {"name":"name","type":"STRING"},{"name":"score","type":"INTEGER"}]},
          {"name":"ts","type":"TIMESTAMP"},{"name":"data","type":"BYTES"}])")}}}});

  const json row = {{"insertId", "row-1"},
                    {"json",
                     {{"id", "1"},
                      {"tags", json::array({"a", "b"})},
                      {"profile", {{"name", "alice"}, {"score", "3"}}},
                      {"ts", "2024-01-02T03:04:05Z"},
                      {"data", "AP8="}}}};
  const json response = Post(base + "/insertAll", {{"rows", json::array({row})}});
  EXPECT_EQ(response["kind"], "bigquery#tableDataInsertAllResponse");
  EXPECT_FALSE(response.contains("insertErrors"));
  const json data = Get(base + "/data");
  EXPECT_EQ(data["rows"][0]["f"][1]["v"], json::parse(R"([{"v":"a"},{"v":"b"}])"));
  EXPECT_EQ(data["rows"][0]["f"][2]["v"]["f"][0]["v"], "alice");
  EXPECT_EQ(data["rows"][0]["f"][3]["v"], "1704164645");
  EXPECT_EQ(data["rows"][0]["f"][4]["v"], "AP8=");
}

}  // namespace
}  // namespace bigquery_emulator_duckdb
