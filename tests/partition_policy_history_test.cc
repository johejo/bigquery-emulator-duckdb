#include "src/partition_policy_history.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "src/api_error.h"
#include "src/backend.h"
#include "src/field_schema.h"
#include "src/partitioning.h"

namespace bigquery_emulator_duckdb {
namespace {

constexpr int64_t kDayMs = 86400000;
constexpr int64_t kStorageId = 42;

int64_t Cutoff() {
  using namespace std::chrono;
  return duration_cast<microseconds>(sys_days{year{2024} / March / 1}.time_since_epoch()).count();
}

class PartitionPolicyHistoryTest : public testing::Test {
 protected:
  void SetUp() override {
    backend_.Execute("ATTACH ':memory:' AS project");
    backend_.Execute(CreatePartitionPolicyHistoryStatement("project"));
    backend_.Execute(
        "CREATE TABLE project.main.rows AS SELECT DATE '2024-02-28' AS d, "
        "'ordinary' AS label UNION ALL SELECT DATE '2024-03-01', 'recent'");
  }

  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): a policy's duration, time and identity.
  void Publish(std::optional<int64_t> duration, int64_t time, int64_t storage_id = kStorageId) {
    const auto session = backend_.NewSession();
    session->Transaction("BEGIN TRANSACTION");
    session->Execute(KeepPartitionPolicyStatement("project", storage_id, duration));
    session->BeforeCommit(CommitPartitionPoliciesStatement("project", time));
    session->Transaction("COMMIT");
  }

  static std::string Labels(Backend& backend, int64_t time, int64_t storage_id = kStorageId) {
    const auto duration = PartitionPolicyAt(backend, "project", storage_id, time);
    const std::string predicate =
        duration ? "NOT (" +
                       ExpiredPartitionPredicate(TimePartitioning{.field = "d"},
                                                 {{.name = "d", .type = FieldType::kDate}},
                                                 *duration, time) +
                       ")"
                 : "true";
    return backend
        .Execute(
            "SELECT coalesce(string_agg(label, ',' ORDER BY label), '') "
            "FROM project.main.rows WHERE " +
            predicate)
        .rows.String(0, 0);
  }

  Backend backend_;
};

// Injecting publication times checks exact microsecond boundaries without sleeps. The known-bug
// history scenarios establish the policy-change workflows; natural deadline passage and script
// transaction policy visibility are not inferred here.
TEST_F(PartitionPolicyHistoryTest, ShortenThenRemoveOrExtendPreservesEarlierPolicies) {
  for (const auto duration : std::vector<std::optional<int64_t>>{std::nullopt, 365 * kDayMs}) {
    const int64_t storage_id = duration ? 43 : kStorageId;
    Publish(kDayMs, Cutoff(), storage_id);
    Publish(duration, Cutoff() + 1000, storage_id);
    EXPECT_EQ(Labels(backend_, Cutoff() - 1, storage_id), "ordinary,recent");
    EXPECT_EQ(Labels(backend_, Cutoff(), storage_id), "recent");
    EXPECT_EQ(Labels(backend_, Cutoff() + 999, storage_id), "recent");
    EXPECT_EQ(Labels(backend_, Cutoff() + 1000, storage_id), "ordinary,recent");
    // Repeated reads must not depend on which historical policy was read last.
    EXPECT_EQ(Labels(backend_, Cutoff(), storage_id), "recent");
  }
}

TEST_F(PartitionPolicyHistoryTest, SpecialPartitionsSurvivePolicyChanges) {
  backend_.Execute(
      "INSERT INTO project.main.rows VALUES (NULL, 'null'), "
      "(DATE '1959-12-31', 'before_range'), (DATE '1960-01-01', 'range_start'), "
      "(DATE '2159-12-31', 'range_end'), (DATE '2160-01-01', 'after_range')");
  Publish(kDayMs, Cutoff());
  Publish(std::nullopt, Cutoff() + 1000);
  EXPECT_EQ(Labels(backend_, Cutoff()), "after_range,before_range,null,range_end,recent");
  EXPECT_EQ(Labels(backend_, Cutoff() + 1000),
            "after_range,before_range,null,ordinary,range_end,range_start,recent");
}

TEST_F(PartitionPolicyHistoryTest, FinalPolicyAndRowsPublishAtomically) {
  const auto session = backend_.NewSession();
  session->Transaction("BEGIN TRANSACTION");
  session->Execute(KeepPartitionPolicyStatement("project", kStorageId, kDayMs));
  session->Execute("INSERT INTO project.main.rows VALUES (DATE '2024-02-27', 'inserted')");
  // Another connection sees neither the staged policy nor the inserted rows.
  EXPECT_EQ(Labels(backend_, Cutoff()), "ordinary,recent");
  // Do not accidentally promise script visibility before it has been verified.
  EXPECT_THROW((void)PartitionPolicyAt(*session, "project", kStorageId, Cutoff()), ApiError);
  session->Execute(KeepPartitionPolicyStatement("project", kStorageId, 365 * kDayMs));
  session->BeforeCommit(CommitPartitionPoliciesStatement("project", Cutoff()));
  session->Transaction("COMMIT");
  EXPECT_EQ(Labels(backend_, Cutoff()), "inserted,ordinary,recent");
  Publish(kDayMs, Cutoff() + 1000);
  EXPECT_EQ(Labels(backend_, Cutoff() + 1000), "recent");
  Publish(std::nullopt, Cutoff() + 2000);
  EXPECT_EQ(Labels(backend_, Cutoff() + 2000), "inserted,ordinary,recent");
}

TEST_F(PartitionPolicyHistoryTest, RollbackPublishesNeitherRowsNorPolicy) {
  Publish(kDayMs, Cutoff());
  const auto session = backend_.NewSession();
  session->Transaction("BEGIN TRANSACTION");
  session->Execute(KeepPartitionPolicyStatement("project", kStorageId, std::nullopt));
  session->Execute("DELETE FROM project.main.rows");
  session->BeforeCommit(CommitPartitionPoliciesStatement("project", Cutoff() + 1000));
  session->Transaction("ROLLBACK");
  EXPECT_EQ(Labels(backend_, Cutoff() + 1000), "recent");
  Publish(std::nullopt, Cutoff() + 2000);
  EXPECT_EQ(Labels(backend_, Cutoff() + 2000), "ordinary,recent");
}

TEST_F(PartitionPolicyHistoryTest, ReplacementDoesNotInheritPolicyEvenWithTheSameName) {
  Publish(kDayMs, Cutoff());
  EXPECT_EQ(Labels(backend_, Cutoff()), "recent");
  // A replacement gets another OID even if its name and millisecond creationTime match.
  EXPECT_EQ(Labels(backend_, Cutoff(), kStorageId + 1), "ordinary,recent");
  Publish(365 * kDayMs, Cutoff() + 1000, kStorageId + 1);
  EXPECT_EQ(Labels(backend_, Cutoff() + 1000), "recent");
  EXPECT_EQ(Labels(backend_, Cutoff() + 1000, kStorageId + 1), "ordinary,recent");
}

TEST_F(PartitionPolicyHistoryTest, InvalidDurationsCannotRemoveExpiration) {
  EXPECT_THROW((void)KeepPartitionPolicyStatement("project", kStorageId, 0), ApiError);
  EXPECT_THROW((void)KeepPartitionPolicyStatement("project", kStorageId, -1), ApiError);
}

}  // namespace
}  // namespace bigquery_emulator_duckdb
