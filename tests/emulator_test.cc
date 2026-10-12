#include "src/emulator.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "nlohmann/json.hpp"
#include "src/api_error.h"
#include "src/backend.h"

namespace bigquery_emulator_duckdb {
namespace {

// Query results are covered by the e2e tests; these cover what clients cannot reach, such as
// options only scripts set and project files written by other means.

// Scripts keep NULL arrays apart from empty ones in their variables; a request from a client never
// sets the option, since BigQuery returns a NULL array as an empty one.
TEST(EmulatorTest, KeepsNullArraysOnRequest) {
  Emulator emulator("", {{.project_id = "test"}});
  QueryRequest request;
  request.project_id = "test";
  request.query = "SELECT CAST(NULL AS ARRAY<INT64>), ARRAY<INT64>[]";
  for (const bool null_arrays : {false, true}) {
    request.null_arrays = null_arrays;
    const auto job = emulator.RunQuery(request);
    if (!job->result.has_value()) {
      FAIL() << (job->error.has_value() ? job->error->what() : "no result");
    }
    const nlohmann::json& row = job->result->rows.at(0)["f"];
    EXPECT_EQ(row[0]["v"].is_null(), null_arrays);
    EXPECT_EQ(row[1]["v"], nlohmann::json::array());
  }
}

// That data survives a restart is covered by tests/e2e/restart; this checks the file layout.
TEST(EmulatorPersistenceTest, StoresEachProjectInItsOwnFile) {
  const std::filesystem::path data_dir =
      std::filesystem::path(::testing::TempDir()) / "emulator_persistence";
  std::filesystem::remove_all(data_dir);
  {
    Emulator emulator(data_dir.string(),
                      {{.project_id = "proj"}, {.project_id = "example.com:proj"}});
    emulator.CreateDataset({.project_id = "proj", .dataset_id = "ds"});
    // A domain-scoped id checks that ':' and '.' are encoded into a single file name.
    emulator.CreateDataset({.project_id = "example.com:proj", .dataset_id = "scoped"});
  }
  EXPECT_TRUE(std::filesystem::exists(data_dir / "proj.duckdb"));
  EXPECT_TRUE(std::filesystem::exists(data_dir / "example%2Ecom%3Aproj.duckdb"));
}

// A view written to the project file by other means has no GoogleSQL definition or schema.
TEST(EmulatorPersistenceTest, HandlesViewsWithoutMetadata) {
  const std::filesystem::path data_dir =
      std::filesystem::path(::testing::TempDir()) / "emulator_foreign_view";
  std::filesystem::remove_all(data_dir);
  std::filesystem::create_directories(data_dir);
  {
    Backend backend;
    backend.Execute("ATTACH '" + (data_dir / "proj.duckdb").string() + "' AS proj");
    backend.Execute("CREATE SCHEMA proj.ds");
    backend.Execute("CREATE VIEW proj.ds.v AS SELECT 1 AS x");
  }
  Emulator emulator(data_dir.string(), {{.project_id = "proj"}});
  const std::vector<TableListEntry> entries =
      emulator.ListTableEntries({.project_id = "proj", .dataset_id = "ds"});
  ASSERT_EQ(entries.size(), 1);
  EXPECT_EQ(entries.at(0).table_id, "v");
  EXPECT_EQ(entries.at(0).type, TableType::kView);
  try {
    emulator.GetTable({.project_id = "proj", .dataset_id = "ds", .table_id = "v"});
    ADD_FAILURE() << "GetTable succeeded";
  } catch (const ApiError& error) {
    EXPECT_EQ(error.http_status(), 400);
    EXPECT_NE(std::string(error.what()).find("was not created by the emulator"), std::string::npos)
        << error.what();
  }

  QueryRequest request;
  request.project_id = "proj";
  request.query =
      "SELECT table_type, ddl IS NULL FROM ds.INFORMATION_SCHEMA.TABLES WHERE table_name = 'v'";
  const std::shared_ptr<const Job> job = emulator.RunQuery(request);
  if (job->error.has_value()) {
    FAIL() << job->error->what();
  }
  if (!job->result.has_value()) {
    FAIL() << "the job has neither a result nor an error";
  }
  const QueryResult& result = *job->result;
  ASSERT_EQ(result.rows.size(), 1);
  EXPECT_EQ(result.rows.at(0)["f"][0]["v"], "VIEW");
  EXPECT_EQ(result.rows.at(0)["f"][1]["v"], "true");

  emulator.DeleteTable({.project_id = "proj", .dataset_id = "ds", .table_id = "v"});
  EXPECT_TRUE(emulator.ListTables({"proj", "ds"}).empty());
}

}  // namespace
}  // namespace bigquery_emulator_duckdb
