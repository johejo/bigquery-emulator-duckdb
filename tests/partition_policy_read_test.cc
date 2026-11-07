#include "src/partition_policy_read.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

#include "src/api_error.h"
#include "src/backend.h"
#include "src/backend_error.h"
#include "src/duckdb_table_source.h"
#include "src/field_schema.h"
#include "src/partition_policy_history.h"
#include "src/partitioning.h"
#include "src/references.h"
#include "src/statement_resources.h"
#include "src/table_comments.h"
#include "src/table_history.h"
#include "src/table_metadata.h"
#include "src/table_source.h"

namespace bigquery_emulator_duckdb {
namespace {

class PartitionPolicyReadTest : public testing::Test {
 protected:
  void SetUp() override {
    using namespace std::chrono;
    cutoff_ =
        duration_cast<microseconds>(system_clock::now().time_since_epoch()).count() - 60000000;
    backend_.Execute("ATTACH ':memory:' AS project");
    backend_.Execute("CREATE SCHEMA project.dataset");
    backend_.Execute(
        "CREATE TABLE project.main.emulator_datasets "
        "(dataset_id VARCHAR, metadata VARCHAR)");
    backend_.ExecuteAll(CreateHistoryStatements("project"));
    // The injected read timestamp and table creation both follow the start of retained history.
    backend_.Execute("UPDATE project.main.emulator_table_history SET valid_to = " +
                     std::to_string(cutoff_ - 60000000) + " WHERE dataset_id IS NULL");
    backend_.Execute(CreatePartitionPolicyHistoryStatement("project"));
    backend_.Execute("CREATE TABLE project.dataset.t (d DATE, label VARCHAR)");
    metadata_.creation_time = (cutoff_ / 1000) - 60000;
    metadata_.time_partitioning = TimePartitioning{.field = "d"};
    backend_.Execute(TableCommentStatement(
        table_, metadata_,
        {{.name = "d", .type = FieldType::kDate}, {.name = "label", .type = FieldType::kString}}));
    backend_.Execute("INSERT INTO project.dataset.t VALUES (DATE '2000-01-01', 'retained')");
    storage_id_ = backend_
                      .Execute(
                          "SELECT table_oid FROM duckdb_tables() WHERE "
                          "database_name = 'project' AND schema_name = 'dataset' "
                          "AND table_name = 't'")
                      .rows.Int64(0, 0);
  }

  std::string Read(Backend& backend, PartitionRowVersion version) {
    auto query = PartitionPolicyTableReadQuery(backend, table_, cutoff_, version);
    if (!query.ok()) {
      ADD_FAILURE() << query.status();
      return "SELECT NULL WHERE FALSE";
    }
    return *query;
  }

  void Publish(std::optional<int64_t> duration, int64_t time) {
    const auto session = backend_.NewSession();
    session->Transaction("BEGIN TRANSACTION");
    session->Execute(KeepPartitionPolicyStatement("project", storage_id_, duration));
    session->BeforeCommit(CommitPartitionPoliciesStatement("project", time));
    session->Transaction("COMMIT");
  }

  std::unique_ptr<TableSource> Source(Backend& backend, int64_t cutoff) {
    return NewDuckDbTableSource(backend, projects_, nullptr, {}, cutoff);
  }

  std::string CurrentRows(TableSource& source) {
    const auto query = StatementResources::ReadQuery(table_, source.ReadQuery(table_), nullptr);
    if (!query) {
      ADD_FAILURE() << "missing partition read definition";
      return "SELECT NULL WHERE FALSE";
    }
    return *query;
  }

  const std::unordered_map<std::string, std::string> projects_{{"project", "project"}};
  Backend backend_;
  TableReference table_{.project_id = "project", .dataset_id = "dataset", .table_id = "t"};
  TableMetadata metadata_;
  int64_t cutoff_ = 0;
  int64_t storage_id_ = 0;
};

TEST_F(PartitionPolicyReadTest, RejectsConstructionWithoutExecutionSnapshot) {
  EXPECT_FALSE(
      PartitionPolicyTableReadQuery(backend_, table_, cutoff_, PartitionRowVersion::kHistorical)
          .ok());
}

// A first write can create the row history relation after analysis. Resolving that relation
// outside the executing snapshot could otherwise read current rows with a historical policy.
TEST_F(PartitionPolicyReadTest, FirstConcurrentWriteCannotChangeResolvedHistoricalRows) {
  const auto reader = backend_.NewSession();
  reader->Transaction("BEGIN TRANSACTION");
  const std::string query = Read(*reader, PartitionRowVersion::kHistorical);
  const auto writer = backend_.NewSession();
  writer->Transaction("BEGIN TRANSACTION");
  writer->ExecuteAll(KeepVersionStatements(table_, metadata_.creation_time));
  writer->Execute("DELETE FROM project.dataset.t");
  writer->BeforeCommit(CommitVersionsStatement("project"));
  writer->Transaction("COMMIT");
  const auto original = reader->Execute(query);
  ASSERT_EQ(original.rows.size(), 1);
  EXPECT_EQ(original.rows.String(0, 1), "retained");
  reader->Transaction("COMMIT");

  const auto later = backend_.NewSession();
  later->Transaction("BEGIN TRANSACTION");
  EXPECT_TRUE(later->Execute(Read(*later, PartitionRowVersion::kCurrent)).rows.empty());
  const auto historical = later->Execute(Read(*later, PartitionRowVersion::kHistorical));
  ASSERT_EQ(historical.rows.size(), 1);
  EXPECT_EQ(historical.rows.String(0, 1), "retained");
  later->Transaction("COMMIT");
}

TEST_F(PartitionPolicyReadTest, ConcurrentPolicyAndRowsCannotMixSnapshots) {
  const auto reader = backend_.NewSession();
  reader->Transaction("BEGIN TRANSACTION");
  const std::string query = Read(*reader, PartitionRowVersion::kCurrent);
  const auto writer = backend_.NewSession();
  writer->Transaction("BEGIN TRANSACTION");
  writer->Execute(KeepPartitionPolicyStatement("project", storage_id_, 86400000));
  writer->Execute("INSERT INTO project.dataset.t VALUES (DATE '2159-12-31', 'new')");
  writer->BeforeCommit(CommitPartitionPoliciesStatement("project", cutoff_));
  writer->Transaction("COMMIT");
  const auto original = reader->Execute(query);
  ASSERT_EQ(original.rows.size(), 1);
  EXPECT_EQ(original.rows.String(0, 1), "retained");
  reader->Transaction("COMMIT");

  const auto later = backend_.NewSession();
  later->Transaction("BEGIN TRANSACTION");
  const auto visible = later->Execute(Read(*later, PartitionRowVersion::kCurrent));
  ASSERT_EQ(visible.rows.size(), 1);
  EXPECT_EQ(visible.rows.String(0, 1), "new");
  later->Transaction("COMMIT");
}

TEST_F(PartitionPolicyReadTest, ReplacementInExecutingTransactionCannotReuseResolvedIdentity) {
  const auto session = backend_.NewSession();
  session->Transaction("BEGIN TRANSACTION");
  const std::string query = Read(*session, PartitionRowVersion::kCurrent);
  session->Execute("DROP TABLE project.dataset.t");
  session->Execute(
      "CREATE TABLE project.dataset.t AS "
      "SELECT DATE '2159-12-31' AS d, 'replacement' AS label");
  EXPECT_THROW((void)session->Execute(query), BackendError);
  session->Transaction("ROLLBACK");
}

TEST_F(PartitionPolicyReadTest, SqlSourceRejectsPolicyReadsWithoutExecutionSnapshot) {
  Publish(86400000, cutoff_ - 1000);
  const auto source = Source(backend_, cutoff_);
  EXPECT_THROW((void)source->ReadQuery(table_), ApiError);
  EXPECT_FALSE(source->TimeTravelQuery(table_, cutoff_).ok());
  EXPECT_THROW((void)source->CountRows(table_, "CAST(d AS VARCHAR)"), ApiError);
}

TEST_F(PartitionPolicyReadTest, SqlPartitionCountsDoNotHideStagedPolicyErrors) {
  const auto session = backend_.NewSession();
  session->Transaction("BEGIN TRANSACTION");
  session->Execute(KeepPartitionPolicyStatement("project", storage_id_, 86400000));
  const auto source = Source(*session, cutoff_);
  EXPECT_THROW((void)source->CountRows(table_, "CAST(d AS VARCHAR)"), BackendError);
  session->Transaction("ROLLBACK");
}

// Inject a commit between SQL source discovery and execution. Both the normal scan and
// INFORMATION_SCHEMA.PARTITIONS counts must keep the discovered snapshot, even when the
// writer creates the first row-history relation and publishes a different expiration policy.
TEST_F(PartitionPolicyReadTest, SqlSourceScansAndPartitionCountsKeepExecutingSnapshot) {
  backend_.Execute("INSERT INTO project.dataset.t VALUES (DATE '2159-12-31', 'future')");
  Publish(86400000, cutoff_);

  const auto reader = backend_.NewSession();
  reader->Transaction("BEGIN TRANSACTION");
  const auto source = Source(*reader, cutoff_);
  const std::string query = CurrentRows(*source);

  const auto writer = backend_.NewSession();
  writer->Transaction("BEGIN TRANSACTION");
  writer->ExecuteAll(KeepVersionStatements(table_, metadata_.creation_time));
  writer->Execute("DELETE FROM project.dataset.t");
  writer->Execute(KeepPartitionPolicyStatement("project", storage_id_, std::nullopt));
  writer->BeforeCommit(CommitVersionsStatement("project"));
  writer->BeforeCommit(CommitPartitionPoliciesStatement("project", cutoff_ + 1000));
  writer->Transaction("COMMIT");

  const auto visible = reader->Execute(query);
  ASSERT_EQ(visible.rows.size(), 1);
  EXPECT_EQ(visible.rows.String(0, 1), "future");
  const auto counts = source->CountRows(table_, "CAST(d AS VARCHAR)");
  ASSERT_TRUE(counts);
  ASSERT_EQ(counts->size(), 1);
  EXPECT_EQ(counts->front().first, "2159-12-31");
  EXPECT_EQ(counts->front().second, 1);
  // Resolving historical rows after the commit must still use the reader's old snapshot.
  const auto historical = source->TimeTravelQuery(table_, cutoff_);
  ASSERT_TRUE(historical.ok()) << historical.status();
  const auto old_rows = reader->Execute(*historical);
  ASSERT_EQ(old_rows.rows.size(), 1);
  EXPECT_EQ(old_rows.rows.String(0, 1), "future");
  reader->Transaction("COMMIT");

  const auto later = backend_.NewSession();
  later->Transaction("BEGIN TRANSACTION");
  const auto later_source = Source(*later, cutoff_ + 1000);
  EXPECT_TRUE(later->Execute(CurrentRows(*later_source)).rows.empty());
  const auto kept = later_source->TimeTravelQuery(table_, cutoff_);
  ASSERT_TRUE(kept.ok()) << kept.status();
  const auto kept_rows = later->Execute(*kept);
  ASSERT_EQ(kept_rows.rows.size(), 1);
  EXPECT_EQ(kept_rows.rows.String(0, 1), "future");
  later->Transaction("COMMIT");
}

TEST_F(PartitionPolicyReadTest, SqlHistoricalReadDoesNotUseCurrentCutoffOrPolicy) {
  Publish(86400000, cutoff_);
  Publish(std::nullopt, cutoff_ + 1000);

  const auto reader = backend_.NewSession();
  reader->Transaction("BEGIN TRANSACTION");
  const auto source = Source(*reader, cutoff_ + 1000);
  EXPECT_EQ(reader->Execute(CurrentRows(*source)).rows.size(), 1);
  const auto historical = source->TimeTravelQuery(table_, cutoff_);
  ASSERT_TRUE(historical.ok()) << historical.status();
  EXPECT_TRUE(reader->Execute(*historical).rows.empty());
  reader->Transaction("COMMIT");
}

}  // namespace
}  // namespace bigquery_emulator_duckdb
