#include "src/sql_runtime.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <barrier>
#include <cstddef>
#include <future>
#include <iterator>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "src/api_error.h"
#include "src/backend.h"
#include "src/location.h"
#include "src/project.h"

namespace bigquery_emulator_duckdb {
namespace {

// Concurrent scripts must not share temporary namespaces or connections. Consume their
// results only after the runtime has gone away, as transports can do with query results.
TEST(SqlRuntimeTest, ConcurrentScriptsAndReturnedResultsHaveIndependentLifetimes) {
  constexpr std::size_t kClients = 8;
  std::array<std::future<QueryResult>, kClients> pending;
  auto runtime = std::make_unique<SqlRuntime>("", std::vector<Project>{{.project_id = "runtime"}});
  std::barrier ready(static_cast<std::ptrdiff_t>(kClients));
  for (std::size_t i = 0; i < kClients; ++i) {
    pending.at(i) = std::async(std::launch::async, [&, i] {
      ready.arrive_and_wait();
      QueryRequest request;
      request.project_id = "runtime";
      request.query = "CREATE TEMP TABLE local_value AS SELECT " + std::to_string(i) +
                      " AS value; SELECT value FROM local_value";
      QueryJob metadata;
      JobLocation location;
      return runtime->ExecuteQuery(std::move(request), metadata, location);
    });
  }
  std::vector<QueryResult> results;
  std::ranges::transform(pending, std::back_inserter(results),
                         [](auto& future) { return future.get(); });
  runtime.reset();
  for (std::size_t i = 0; i < kClients; ++i) {
    ASSERT_EQ(results.at(i).rows.size(), 1);
    EXPECT_EQ(results.at(i).rows.Int64(0, 0), i);
  }
}

// Provider absence is a runtime composition choice, unavailable through production's API.
// Both array and scalar fetch forms must be rejected before calling an absent callback.
TEST(SqlRuntimeTest, ObjectMetadataFetchRequiresAnInstalledCapability) {
  SqlRuntime runtime("", std::vector<Project>{{.project_id = "runtime"}});
  for (const char* expression : {
           "OBJ.MAKE_REF('gs://bucket/object', 'runtime.us.connection')",
           "[OBJ.MAKE_REF('gs://bucket/object', 'runtime.us.connection')]",
       }) {
    QueryRequest request;
    request.project_id = "runtime";
    request.query = std::string("SELECT OBJ.FETCH_METADATA(") + expression + ")";
    QueryJob metadata;
    JobLocation location;
    try {
      runtime.ExecuteQuery(std::move(request), metadata, location);
      FAIL() << "fetch succeeded without a provider";
    } catch (const ApiError& error) {
      EXPECT_TRUE(std::string(error.what()).starts_with(kUnsupported)) << error.what();
    }
  }
}

}  // namespace
}  // namespace bigquery_emulator_duckdb
