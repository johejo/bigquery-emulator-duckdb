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

TEST_F(ServerTest, RunsQuery) {
  const json response =
      Post("/bigquery/v2/projects/p/queries", {{"query", "SELECT 1 AS x, 'a' AS y"}});
  EXPECT_EQ(response["kind"], "bigquery#queryResponse");
  EXPECT_EQ(response["jobComplete"], true);
  EXPECT_EQ(response["totalRows"], "1");
  EXPECT_EQ(response["schema"]["fields"][0]["name"], "x");
  EXPECT_EQ(response["rows"], json::parse(R"([{"f": [{"v": "1"}, {"v": "a"}]}])"));
}

TEST_F(ServerTest, ReportsQueryErrors) {
  const json response = Post("/bigquery/v2/projects/p/queries", {{"query", "SELECT * FROM nope"}});
  ASSERT_TRUE(response.contains("errors"));
  EXPECT_EQ(response["errors"][0]["reason"], "invalidQuery");
}

TEST_F(ServerTest, RunsJobAndFetchesResults) {
  const json job = Post("/bigquery/v2/projects/p/jobs",
                        {{"jobReference", {{"projectId", "p"}, {"jobId", "job1"}}},
                         {"configuration", {{"query", {{"query", "SELECT 1"}}}}}});
  EXPECT_EQ(job["jobReference"]["jobId"], "job1");
  EXPECT_EQ(job["status"]["state"], "DONE");
  EXPECT_EQ(job["statistics"]["query"]["statementType"], "SELECT");

  EXPECT_EQ(Get("/bigquery/v2/projects/p/jobs/job1")["status"]["state"], "DONE");
  const json results = Get("/bigquery/v2/projects/p/queries/job1");
  EXPECT_EQ(results["kind"], "bigquery#getQueryResultsResponse");
  EXPECT_EQ(results["rows"][0]["f"][0]["v"], "1");
  Get("/bigquery/v2/projects/p/queries/missing", 404);
}

TEST_F(ServerTest, WritesJobResultsToADestinationTable) {
  Post("/bigquery/v2/projects/p/datasets", {{"datasetReference", {{"datasetId", "ds"}}}});
  const json destination = {{"datasetId", "ds"}, {"tableId", "t"}};
  const json job =
      Post("/bigquery/v2/projects/p/jobs",
           {{"jobReference", {{"jobId", "job1"}}},
            {"configuration",
             {{"query", {{"query", "SELECT 1 AS x"}, {"destinationTable", destination}}}}}});
  EXPECT_EQ(job["status"]["state"], "DONE");
  const json& query = job["configuration"]["query"];
  EXPECT_EQ(query["destinationTable"]["projectId"], "p");
  EXPECT_EQ(query["destinationTable"]["tableId"], "t");
  EXPECT_EQ(query["createDisposition"], "CREATE_IF_NEEDED");
  EXPECT_EQ(query["writeDisposition"], "WRITE_EMPTY");
  EXPECT_EQ(Get("/bigquery/v2/projects/p/queries/job1")["rows"][0]["f"][0]["v"], "1");
  EXPECT_EQ(Get("/bigquery/v2/projects/p/datasets/ds/tables/t/data")["rows"][0]["f"][0]["v"], "1");

  const json again =
      Post("/bigquery/v2/projects/p/jobs",
           {{"configuration",
             {{"query", {{"query", "SELECT 2 AS x"}, {"destinationTable", destination}}}}}});
  EXPECT_EQ(again["status"]["errorResult"]["reason"], "duplicate");
}

TEST_F(ServerTest, PaginatesQueryResults) {
  Post("/bigquery/v2/projects/p/jobs",
       {{"jobReference", {{"jobId", "job1"}}},
        {"configuration", {{"query", {{"query", "SELECT * FROM UNNEST([1, 2, 3]) AS x"}}}}}});
  json page = Get("/bigquery/v2/projects/p/queries/job1?maxResults=2");
  EXPECT_EQ(page["totalRows"], "3");
  EXPECT_EQ(page["rows"].size(), 2);
  EXPECT_EQ(page["pageToken"], "2");
  page = Get("/bigquery/v2/projects/p/queries/job1?maxResults=2&pageToken=2");
  EXPECT_EQ(page["rows"].size(), 1);
  EXPECT_FALSE(page.contains("pageToken"));
}

TEST_F(ServerTest, RunsQueryWithNamedParameters) {
  const json response =
      Post("/bigquery/v2/projects/p/queries", {{"query", "SELECT @n AS n, @s AS s, @a AS a"},
                                               {"parameterMode", "NAMED"},
                                               {"queryParameters", json::parse(R"([
                {"name": "n", "parameterType": {"type": "INT64"},
                 "parameterValue": {"value": "7"}},
                {"name": "s", "parameterType": {"type": "STRING"},
                 "parameterValue": {"value": "x"}},
                {"name": "a", "parameterType": {"type": "ARRAY",
                                                "arrayType": {"type": "INT64"}},
                 "parameterValue": {"arrayValues": [{"value": "1"}, {"value": "2"}]}}])")}});
  EXPECT_EQ(response["schema"]["fields"][2]["mode"], "REPEATED");
  EXPECT_EQ(response["rows"][0]["f"],
            json::parse(R"([{"v": "7"}, {"v": "x"}, {"v": [{"v": "1"}, {"v": "2"}]}])"));
}

TEST_F(ServerTest, RunsQueryWithPositionalParameters) {
  const json response =
      Post("/bigquery/v2/projects/p/queries", {{"query", "SELECT ? AS a, ? AS b"},
                                               {"parameterMode", "POSITIONAL"},
                                               {"queryParameters", json::parse(R"([
                {"parameterType": {"type": "INT64"}, "parameterValue": {"value": "1"}},
                {"parameterType": {"type": "STRING"}, "parameterValue": {"value": "b"}}])")}});
  EXPECT_EQ(response["rows"][0]["f"], json::parse(R"([{"v": "1"}, {"v": "b"}])"));
}

TEST_F(ServerTest, ReportsUndeclaredParameters) {
  const json response = Post("/bigquery/v2/projects/p/queries", {{"query", "SELECT @missing"}});
  ASSERT_TRUE(response.contains("errors"));
  EXPECT_EQ(response["errors"][0]["reason"], "invalidQuery");
}

TEST_F(ServerTest, ValidatesQueriesWithoutRunningThem) {
  Post("/bigquery/v2/projects/p/datasets", {{"datasetReference", {{"datasetId", "ds"}}}});
  Post("/bigquery/v2/projects/p/queries", {{"query", "CREATE TABLE ds.t (id INT64)"}});

  const json response = Post("/bigquery/v2/projects/p/queries",
                             {{"query", "SELECT id FROM `p.ds.t`"}, {"dryRun", true}});
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
  EXPECT_EQ(job["status"]["state"], "DONE");
  EXPECT_EQ(job["configuration"]["dryRun"], true);
  EXPECT_FALSE(job["status"].contains("errorResult"));
  Get("/bigquery/v2/projects/p/jobs/dry1", 404);
  EXPECT_EQ(Post("/bigquery/v2/projects/p/queries",
                 {{"query", "SELECT count(*) AS c FROM `p.ds.t`"}})["rows"][0]["f"][0]["v"],
            "0");

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

TEST_F(ServerTest, ManagesDatasetsAndTables) {
  Post("/bigquery/v2/projects/p/datasets", {{"datasetReference", {{"datasetId", "ds"}}}});
  Post("/bigquery/v2/projects/p/datasets", {{"datasetReference", {{"datasetId", "ds"}}}}, 409);
  EXPECT_EQ(Get("/bigquery/v2/projects/p/datasets")["datasets"][0]["id"], "p:ds");
  EXPECT_EQ(Get("/bigquery/v2/projects/p/datasets/ds")["datasetReference"]["datasetId"], "ds");
  Get("/bigquery/v2/projects/p/datasets/missing", 404);

  const json table = Post(
      "/bigquery/v2/projects/p/datasets/ds/tables",
      {{"tableReference", {{"tableId", "t"}}},
       {"schema", {{"fields", json::parse(R"([{"name": "id", "type": "INTEGER", "mode": "REQUIRED"},
                                    {"name": "tags", "type": "STRING", "mode": "REPEATED"},
                                    {"name": "ts", "type": "TIMESTAMP"}])")}}}});
  EXPECT_EQ(table["id"], "p:ds.t");
  EXPECT_EQ(table["schema"]["fields"][1]["mode"], "REPEATED");
  EXPECT_EQ(table["schema"]["fields"][2]["type"], "TIMESTAMP");
  EXPECT_EQ(Get("/bigquery/v2/projects/p/datasets/ds/tables")["tables"][0]["id"], "p:ds.t");

  Post("/bigquery/v2/projects/p/queries",
       {{"query", "INSERT INTO ds.t VALUES (1, ['a'], TIMESTAMP '2020-01-01 00:00:00+00')"}});
  const json data = Get("/bigquery/v2/projects/p/datasets/ds/tables/t/data");
  EXPECT_EQ(data["totalRows"], "1");
  EXPECT_EQ(data["rows"][0]["f"][2]["v"], "1577836800");

  const json query = Post("/bigquery/v2/projects/p/queries",
                          {{"query", "SELECT id FROM `p.ds.t`"},
                           {"defaultDataset", {{"projectId", "p"}, {"datasetId", "ds"}}}});
  EXPECT_EQ(query["rows"][0]["f"][0]["v"], "1");

  EXPECT_EQ(client_->Delete("/bigquery/v2/projects/p/datasets/ds")->status, 400);
  EXPECT_EQ(client_->Delete("/bigquery/v2/projects/p/datasets/ds/tables/t")->status, 204);
  EXPECT_EQ(client_->Delete("/bigquery/v2/projects/p/datasets/ds")->status, 204);
  Get("/bigquery/v2/projects/p/datasets/ds", 404);
}

TEST_F(ServerTest, StreamsRowsAndReportsRowErrors) {
  const std::string base = "/projects/p/datasets/ds/tables/t";
  Post("/projects/p/datasets", {{"datasetReference", {{"datasetId", "ds"}}}});
  Post("/projects/p/datasets/ds/tables",
       {{"tableReference", {{"tableId", "t"}}}, {"schema", {{"fields", json::parse(R"([
          {"name":"id","type":"INTEGER","mode":"REQUIRED"},
          {"name":"tags","type":"STRING","mode":"REPEATED"},
          {"name":"profile","type":"RECORD","fields":[
            {"name":"name","type":"STRING"},{"name":"score","type":"INTEGER"}]},
          {"name":"ts","type":"TIMESTAMP"},{"name":"data","type":"BYTES"}])")}}}});

  const json good = {{"insertId", "row-1"},
                     {"json",
                      {{"id", "1"},
                       {"tags", json::array({"a", "b"})},
                       {"profile", {{"name", "alice"}, {"score", "3"}}},
                       {"ts", "2024-01-02T03:04:05Z"},
                       {"data", "AP8="}}}};
  const json bad = {{"json", {{"id", "nope"}}}};
  json response = Post(base + "/insertAll", {{"rows", json::array({good, bad})}});
  ASSERT_EQ(response["insertErrors"].size(), 1);
  EXPECT_EQ(response["insertErrors"][0]["index"], 1);
  EXPECT_EQ(Get(base + "/data")["totalRows"], "0");

  response =
      Post(base + "/insertAll", {{"rows", json::array({good, bad})}, {"skipInvalidRows", true}});
  ASSERT_EQ(response["insertErrors"].size(), 1);
  EXPECT_EQ(response["insertErrors"][0]["index"], 1);
  const json data = Get(base + "/data");
  EXPECT_EQ(data["totalRows"], "1");
  EXPECT_EQ(data["rows"][0]["f"][1]["v"], json::parse(R"([{"v":"a"},{"v":"b"}])"));
  EXPECT_EQ(data["rows"][0]["f"][2]["v"]["f"][0]["v"], "alice");
  EXPECT_EQ(data["rows"][0]["f"][3]["v"], "1704164645");
  EXPECT_EQ(data["rows"][0]["f"][4]["v"], "AP8=");

  response = Post(base + "/insertAll",
                  {{"rows", json::array({{{"json", {{"id", 2}, {"extra", "ignored"}}}}})},
                   {"ignoreUnknownValues", true}});
  EXPECT_FALSE(response.contains("insertErrors"));
  EXPECT_EQ(Get(base + "/data")["totalRows"], "2");
}

}  // namespace
}  // namespace bigquery_emulator_duckdb
