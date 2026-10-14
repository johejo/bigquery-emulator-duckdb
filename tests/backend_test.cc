#include "src/backend.h"

#include <future>

#include "gtest/gtest.h"
#include "nlohmann/json.hpp"
#include "src/backend_error.h"

namespace bigquery_emulator_duckdb {
namespace {

TEST(BackendTest, SessionIdentitiesAreIsolatedAcrossConcurrentDatabases) {
  Backend first("first@example.com");
  Backend second("second@example.com");
  const auto read = [](Backend& backend) {
    auto session = backend.NewSession();
    session->Transaction("BEGIN TRANSACTION");
    const QueryResult result = session->Execute("SELECT bq_session_user()");
    session->Transaction("COMMIT");
    return result.rows.at(0).at("f").at(0).at("v");
  };
  auto a = std::async(std::launch::async, [&] { return read(first); });
  auto b = std::async(std::launch::async, [&] { return read(second); });
  EXPECT_EQ(a.get(), "first@example.com");
  EXPECT_EQ(b.get(), "second@example.com");
}

TEST(BackendTest, RollsBackViewCreationWhenMetadataFails) {
  Backend backend;
  EXPECT_THROW(backend.ExecuteDdl("CREATE VIEW v AS SELECT 1 AS x",
                                  {"COMMENT ON VIEW missing IS 'metadata'"}, ""),
               BackendError);
  EXPECT_THROW(backend.Prepare("SELECT * FROM v"), BackendError);
  backend.ExecuteDdl("CREATE VIEW v AS SELECT 2 AS x", {"COMMENT ON VIEW v IS 'original'"}, "");
  EXPECT_THROW(backend.ExecuteDdl("CREATE OR REPLACE VIEW v AS SELECT 3 AS y",
                                  {"COMMENT ON VIEW missing IS 'metadata'"}, ""),
               BackendError);
  const QueryResult metadata =
      backend.Execute("SELECT comment FROM duckdb_views() WHERE view_name = 'v'");
  ASSERT_EQ(metadata.rows.size(), 1);
  EXPECT_EQ(metadata.rows.at(0)["f"][0]["v"], "original");
  EXPECT_EQ(backend.Prepare("SELECT * FROM v").schema.at(0).name, "x");
}

}  // namespace
}  // namespace bigquery_emulator_duckdb
