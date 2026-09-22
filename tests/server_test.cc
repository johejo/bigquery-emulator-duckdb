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

}  // namespace
}  // namespace bigquery_emulator_duckdb
