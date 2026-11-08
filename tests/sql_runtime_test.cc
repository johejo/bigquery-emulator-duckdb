#include "src/sql_runtime.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <barrier>
#include <cstddef>
#include <cstdint>
#include <future>
#include <iterator>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "nlohmann/json.hpp"
#include "src/api_error.h"
#include "src/backend.h"
#include "src/field_schema.h"
#include "src/location.h"
#include "src/project.h"
#include "src/references.h"
#include "src/table_metadata.h"

namespace bigquery_emulator_duckdb {
namespace {

// Atomically replaces the rows and partition policy of runtime.policy.t, which is partitioned
// by d. With an expiration of one day, 2000-01-01 is expired and 2159-12-31 is not, so a read
// that evaluates the rows it sees with the policy published with them always counts one row.
void PublishPolicy(SqlRuntime& runtime, bool expires) {
  const std::string duration = expires ? "86400000" : "NULL";
  runtime.ExecuteDuckDb(
      "BEGIN; DELETE FROM runtime.policy.t; "
      "INSERT INTO runtime.policy.t VALUES (DATE '2000-01-01'); " +
      std::string(expires ? "INSERT INTO runtime.policy.t VALUES (DATE '2159-12-31'); " : "") +
      "INSERT INTO runtime.main.emulator_partition_policy_history "
      "SELECT table_oid, txid_current(), epoch_us(get_current_timestamp()), " +
      duration +
      " FROM duckdb_tables() WHERE database_name = 'runtime' "
      "AND schema_name = 'policy' AND table_name = 't'; COMMIT");
}

// Publishes the expiring policy right after a read takes its evaluation time, before it
// returns it: the read's snapshot must already exclude what is committed then.
class PublishingAtEvaluationRuntime : public SqlRuntime {
 public:
  PublishingAtEvaluationRuntime()
      : SqlRuntime("", std::vector<Project>{{.project_id = "runtime"}}) {}

  void PublishAtNextEvaluation() { publish_ = true; }

 protected:
  int64_t PolicyEvaluationMicros() override {
    const int64_t now = SqlRuntime::PolicyEvaluationMicros();
    if (std::exchange(publish_, false)) {
      PublishPolicy(*this, true);
    }
    return now;
  }

 private:
  bool publish_ = false;
};

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

// Inject atomic row/policy changes while both normal SQL and script expressions read. A
// reader that resolves policy or raw rows outside its execution snapshot can report 0 or 2.
TEST(SqlRuntimeTest, SqlReadsKeepRowsAndPolicyInOneSnapshot) {
  SqlRuntime runtime("", std::vector<Project>{{.project_id = "runtime"}});
  runtime.CreateDataset({.project_id = "runtime", .dataset_id = "policy"});
  const auto query = [&](const std::string& sql) {
    QueryRequest request;
    request.project_id = "runtime";
    request.query = sql;
    QueryJob metadata;
    JobLocation location;
    return runtime.ExecuteQuery(std::move(request), metadata, location);
  };
  query("CREATE TABLE policy.t PARTITION BY d AS SELECT DATE '2000-01-01' AS d");
  const auto publish = [&](bool expires) { PublishPolicy(runtime, expires); };
  publish(false);
  constexpr int kChanges = 64;
  std::barrier ready(2);
  auto writer = std::async(std::launch::async, [&] {
    ready.arrive_and_wait();
    for (int i = 0; i < kChanges; ++i) {
      publish(i % 2 == 0);
    }
  });
  ready.arrive_and_wait();
  for (int i = 0; i < kChanges; ++i) {
    const auto result =
        query(i % 2 == 0 ? "SELECT count(*) FROM policy.t"
                         : "DECLARE n INT64 DEFAULT (SELECT count(*) FROM policy.t); SELECT n");
    ASSERT_EQ(result.rows.size(), 1);
    EXPECT_EQ(result.rows.Int64(0, 0), 1);
  }
  EXPECT_NO_THROW(writer.get());
}

// A policy committed between a read's evaluation time and its first read of the project
// must not be dated after that time while its rows are visible, in queries, script expressions
// or the table API.
TEST(SqlRuntimeTest, PolicyCommittedAtEvaluationTimeIsOutsideTheReadSnapshot) {
  PublishingAtEvaluationRuntime runtime;
  runtime.CreateDataset({.project_id = "runtime", .dataset_id = "policy"});
  const auto query = [&](const std::string& sql) {
    QueryRequest request;
    request.project_id = "runtime";
    request.query = sql;
    QueryJob metadata;
    JobLocation location;
    return runtime.ExecuteQuery(std::move(request), metadata, location);
  };
  query("CREATE TABLE policy.t PARTITION BY d AS SELECT DATE '2000-01-01' AS d");
  for (const std::string sql : {
           "SELECT count(*) FROM policy.t",
           "DECLARE n INT64 DEFAULT (SELECT count(*) FROM policy.t); SELECT n",
       }) {
    PublishPolicy(runtime, false);
    runtime.PublishAtNextEvaluation();
    const auto result = query(sql);
    ASSERT_EQ(result.rows.size(), 1) << sql;
    EXPECT_EQ(result.rows.Int64(0, 0), 1) << sql;
  }
  PublishPolicy(runtime, false);
  runtime.PublishAtNextEvaluation();
  EXPECT_EQ(
      runtime.GetTable({.project_id = "runtime", .dataset_id = "policy", .table_id = "t"}).num_rows,
      1);
}

// Catalog reads in another dataset may expire a newly committed relation while its CREATE
// response is still being assembled. Exercise SQL and API mutations, including nested view
// analysis and table reads, without relying on wall-clock waits.
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
        std::vector<FieldSchema> schema;
        if (n % 4 < 2) {
          QueryRequest request;
          request.project_id = "runtime";
          request.query = "CREATE " + std::string(n % 2 == 0 ? "VIEW " : "TABLE ") + dataset +
                          ".expired_" + std::to_string(n) +
                          " OPTIONS (expiration_timestamp = TIMESTAMP '2001-01-01 00:00:00 UTC')"
                          " AS SELECT CAST(7 AS INT64) AS value";
          QueryJob metadata;
          JobLocation location;
          schema = runtime.ExecuteQuery(std::move(request), metadata, location).schema;
        } else {
          const TableReference table{
              .project_id = "runtime",
              .dataset_id = dataset,
              .table_id = "expired_" + std::to_string(n),
          };
          // The same fixed, already-expired deadline as the SQL form, in epoch milliseconds.
          TableMetadata metadata;
          metadata.expiration_time = 978307200000;
          if (n % 4 == 2) {
            schema =
                runtime
                    .CreateTable(table, {{.name = "value", .type = FieldType::kInteger}}, metadata)
                    .schema;
          } else {
            schema = runtime
                         .CreateView(table,
                                     nlohmann::json{
                                         {"query", "SELECT CAST(7 AS INT64) AS value"},
                                         {"useLegacySql", false},
                                     },
                                     metadata)
                         .schema;
          }
        }
        ASSERT_EQ(schema.size(), 1);
        EXPECT_EQ(schema.front().name, "value");
        EXPECT_EQ(schema.front().type, FieldType::kInteger);
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
