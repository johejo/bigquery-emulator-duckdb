#include "src/backend.h"

#include <future>
#include <memory>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "src/result_rows.h"
#include "src/storage_snapshot.h"

namespace bigquery_emulator_duckdb {
namespace {

// Cross chunk boundaries and use non-inline strings so reads exercise retained DuckDB buffers.
constexpr char kRows[] =
    "SELECT i::BIGINT AS value, repeat('x', 100) AS payload FROM range(4097) t(i) ORDER BY i";

TEST(BackendTest, SessionOutlivesItsOriginalBackend) {
  std::unique_ptr<Backend> session;
  {
    Backend backend;
    session = backend.NewSession();
    session->Execute("CREATE TEMP TABLE pending AS SELECT 7::BIGINT AS value");
    session->Transaction("BEGIN TRANSACTION");
    session->Execute("INSERT INTO pending VALUES (8)");
  }
  session->Transaction("COMMIT");
  const auto result = session->Execute("SELECT value FROM pending ORDER BY value");
  ASSERT_EQ(result.rows.size(), 2);
  EXPECT_EQ(result.rows.Int64(0, 0), 7);
  EXPECT_EQ(result.rows.Int64(1, 0), 8);
}

TEST(BackendTest, RowsOutliveBackendAndSessionAndCanBeReadConcurrently) {
  ResultRows rows;
  {
    Backend backend;
    const auto session = backend.NewSession();
    rows = session->Execute(kRows).rows;
  }
  ASSERT_EQ(rows.size(), 4097);
  std::vector<std::future<void>> readers;
  readers.reserve(4);
  for (int reader = 0; reader < 4; ++reader) {
    readers.push_back(std::async(std::launch::async, [rows] {
      for (const auto row : {0, 2048, 4096}) {
        EXPECT_EQ(rows.Int64(row, 0), row);
        EXPECT_EQ(rows.String(row, 1), std::string(100, 'x'));
      }
    }));
  }
  rows = {};
  for (auto& reader : readers) {
    reader.get();
  }
}

TEST(BackendTest, StorageSnapshotOutlivesItsOriginalBackend) {
  std::shared_ptr<const StorageSnapshot> snapshot;
  {
    Backend backend;
    const auto schema = backend.Prepare(kRows).schema;
    snapshot = backend.ExecuteStorageRead(kRows, schema);
  }
  ASSERT_EQ(snapshot->row_count(), 4097);
  // Both encodings must still be able to read the retained chunks and session settings.
  EXPECT_FALSE(snapshot->ReadRows(2048, 1, false).empty());
  EXPECT_FALSE(snapshot->ReadRows(4096, 1, true).empty());
}

}  // namespace
}  // namespace bigquery_emulator_duckdb
