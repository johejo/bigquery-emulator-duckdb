#include "src/partition_policy_read.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <string>

#include "src/backend.h"
#include "src/backend_error.h"
#include "src/field_schema.h"
#include "src/partition_policy_history.h"
#include "src/partitioning.h"
#include "src/references.h"
#include "src/table_comments.h"
#include "src/table_history.h"
#include "src/table_metadata.h"

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

}  // namespace
}  // namespace bigquery_emulator_duckdb
