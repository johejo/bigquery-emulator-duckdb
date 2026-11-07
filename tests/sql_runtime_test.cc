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
#include "src/field_schema.h"
#include "src/location.h"
#include "src/project.h"
#include "src/references.h"

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

// Catalog reads in another dataset may expire a newly committed relation while its CREATE
// response is still being assembled. Exercise that overlap without relying on wall-clock waits.
TEST(SqlRuntimeTest, ConcurrentExpirationDoesNotFailCreateResponses) {
  constexpr std::size_t kClients = 8;
  constexpr std::size_t kCreates = 64;
  SqlRuntime runtime("", std::vector<Project>{{.project_id = "runtime"}});
  for (std::size_t i = 0; i < kClients; ++i) {
    runtime.CreateDataset(
        {.project_id = "runtime", .dataset_id = "expiration_" + std::to_string(i)});
  }
  std::barrier ready(static_cast<std::ptrdiff_t>(kClients));
  std::array<std::future<void>, kClients> pending;
  for (std::size_t i = 0; i < kClients; ++i) {
    pending.at(i) = std::async(std::launch::async, [&, i] {
      const std::string dataset = "expiration_" + std::to_string(i);
      ready.arrive_and_wait();
      for (std::size_t n = 0; n < kCreates; ++n) {
        QueryRequest request;
        request.project_id = "runtime";
        request.query = "CREATE " + std::string(n % 2 == 0 ? "VIEW " : "TABLE ") + dataset +
                        ".expired_" + std::to_string(n) +
                        " OPTIONS (expiration_timestamp = TIMESTAMP '2001-01-01 00:00:00 UTC')"
                        " AS SELECT CAST(7 AS INT64) AS value";
        QueryJob metadata;
        JobLocation location;
        const QueryResult result = runtime.ExecuteQuery(std::move(request), metadata, location);
        ASSERT_EQ(result.schema.size(), 1);
        EXPECT_EQ(result.schema.front().name, "value");
        EXPECT_EQ(result.schema.front().type, FieldType::kInteger);
      }
    });
  }
  for (auto& future : pending) {
    EXPECT_NO_THROW(future.get());
  }
  for (std::size_t i = 0; i < kClients; ++i) {
    EXPECT_TRUE(
        runtime
            .ListTables({.project_id = "runtime", .dataset_id = "expiration_" + std::to_string(i)})
            .empty());
  }
}

// A replacement can commit between metadata discovery and row execution. API readers must
// neither bind the previous schema against the replacement nor report its row count with the
// previous metadata. Alternating partitioned schemas also exercises reads with no policy yet.
TEST(SqlRuntimeTest, TableApiReadsUseOneSnapshotAcrossConcurrentReplacement) {
  SqlRuntime runtime("", std::vector<Project>{{.project_id = "runtime"}});
  runtime.CreateDataset({.project_id = "runtime", .dataset_id = "reads"});
  const TableReference table{
      .project_id = "runtime",
      .dataset_id = "reads",
      .table_id = "replaced",
  };
  const auto replace = [&](int value) {
    const std::string name = value % 2 == 0 ? "even" : "odd";
    const std::string row =
        "SELECT DATE '2000-01-01' AS d, " + std::to_string(value) + " AS " + name;
    QueryRequest request;
    request.project_id = "runtime";
    request.query = "CREATE OR REPLACE TABLE reads.replaced PARTITION BY d AS " + row +
                    (value % 2 == 0 ? " UNION ALL " + row : "");
    QueryJob metadata;
    JobLocation location;
    runtime.ExecuteQuery(std::move(request), metadata, location);
  };
  replace(0);
  constexpr int kReaders = 4;
  constexpr int kReplacements = 64;
  std::barrier ready(kReaders + 1);
  auto writer = std::async(std::launch::async, [&] {
    ready.arrive_and_wait();
    for (int value = 1; value <= kReplacements; ++value) {
      replace(value);
    }
  });
  std::array<std::future<void>, kReaders> readers;
  for (auto& reader : readers) {
    reader = std::async(std::launch::async, [&] {
      ready.arrive_and_wait();
      for (int n = 0; n < kReplacements; ++n) {
        const TableInfo info = runtime.GetTable(table);
        ASSERT_EQ(info.schema.size(), 2);
        EXPECT_EQ(info.num_rows, info.schema.at(1).name == "even" ? 2 : 1);
        const auto page = runtime.ListTableData(table, 0, 1, "");
        const QueryResult& data = page.result;
        ASSERT_EQ(data.schema.size(), 2);
        const bool even = data.schema.at(1).name == "even";
        EXPECT_EQ(page.total_rows, even ? 2 : 1);
        ASSERT_EQ(data.rows.size(), 1);
        EXPECT_EQ(data.rows.Int64(0, 1) % 2, even ? 0 : 1);
      }
    });
  }
  EXPECT_NO_THROW(writer.get());
  for (auto& reader : readers) {
    EXPECT_NO_THROW(reader.get());
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
