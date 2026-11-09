#include "src/expiration_policy.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <optional>

#include "src/api_error.h"

namespace bigquery_emulator_duckdb {
namespace {

constexpr int64_t kDayMs = 86400000;
constexpr int64_t kWeekMs = 7 * kDayMs;
constexpr int64_t kCreated = 1709251200000;
constexpr int64_t kDeadline = kCreated + (30 * kDayMs);

ExpirationPolicy Policy(std::optional<int64_t> deadline, std::optional<int64_t> duration) {
  return {.table_deadline_ms = deadline, .partition_duration_ms = duration};
}

// Creation policy expectations come from the dataset specification and recorded
// partition_expiration_policy/ddl API answers; see docs/expiration.md. These checks exercise
// policy resolution, not runtime support.
// Creation without partition defaults is covered through the public API and DDL in
// tests/e2e/goclient/dataset_default_expiration_test.go.
TEST(ExpirationPolicyTest, CreationWithPartitionDefaults) {
  EXPECT_EQ(ResolveCreationExpiration(false, kCreated, {}, kWeekMs, kDayMs),
            Policy(kCreated + kWeekMs, std::nullopt));
  EXPECT_EQ(ResolveCreationExpiration(false, kCreated, {}, std::nullopt, kDayMs),
            Policy(std::nullopt, std::nullopt));
  EXPECT_EQ(ResolveCreationExpiration(true, kCreated, {}, std::nullopt, kDayMs),
            Policy(std::nullopt, kDayMs));
  EXPECT_EQ(ResolveCreationExpiration(true, kCreated, {}, kWeekMs, kDayMs),
            Policy(std::nullopt, kDayMs));
}

TEST(ExpirationPolicyTest, ExplicitSettingsAndDatasetDefaults) {
  const auto requested = Policy(std::nullopt, 2 * kDayMs);
  EXPECT_EQ(ResolveCreationExpiration(true, kCreated, requested, std::nullopt, kDayMs), requested);
  EXPECT_EQ(ResolveCreationExpiration(true, kCreated, requested, kWeekMs, kDayMs), requested);
  // An explicit partition duration does not suppress the table default without DP.
  EXPECT_EQ(ResolveCreationExpiration(true, kCreated, requested, kWeekMs, std::nullopt),
            Policy(kCreated + kWeekMs, 2 * kDayMs));
  EXPECT_EQ(
      ResolveCreationExpiration(true, kCreated, Policy(kDeadline, std::nullopt), kWeekMs, kDayMs),
      Policy(kDeadline, kDayMs));
}

TEST(ExpirationPolicyTest, DatasetChangesDoNotRewriteAnExistingEffectivePolicy) {
  const ExpirationPolicy requested;
  const auto old = ResolveCreationExpiration(true, kCreated, requested, kWeekMs, kDayMs);
  const auto changed = ResolveCreationExpiration(true, kCreated, requested, kWeekMs, 2 * kDayMs);
  const auto cleared = ResolveCreationExpiration(true, kCreated, requested, kWeekMs, std::nullopt);
  EXPECT_EQ(old, Policy(std::nullopt, kDayMs));
  EXPECT_EQ(changed, Policy(std::nullopt, 2 * kDayMs));
  EXPECT_EQ(cleared, Policy(kCreated + kWeekMs, std::nullopt));
}

TEST(ExpirationPolicyTest, InvalidPartitionDurationsAreNotRemovalSpellings) {
  for (const int64_t duration : {int64_t{0}, int64_t{-1}}) {
    EXPECT_THROW((void)ResolveCreationExpiration(true, kCreated, Policy(std::nullopt, duration),
                                                 std::nullopt, std::nullopt),
                 ApiError);
    EXPECT_THROW((void)ResolveCreationExpiration(true, kCreated, {}, std::nullopt, duration),
                 ApiError);
  }
}

TEST(ExpirationPolicyTest, TableDeadlineArithmeticDoesNotOverflow) {
  constexpr auto kMax = std::numeric_limits<int64_t>::max();
  EXPECT_EQ(
      ResolveCreationExpiration(false, kMax - kDayMs, {}, kDayMs, std::nullopt).table_deadline_ms,
      kMax);
  EXPECT_THROW((void)ResolveCreationExpiration(false, kMax - kDayMs + 1, {}, kDayMs, std::nullopt),
               ApiError);
  // Do not evaluate a table default that a partition default suppresses.
  EXPECT_EQ(ResolveCreationExpiration(true, kMax, {}, kDayMs, kDayMs),
            Policy(std::nullopt, kDayMs));
  // An explicit deadline also avoids evaluating the dataset's table default.
  EXPECT_EQ(
      ResolveCreationExpiration(false, kMax, Policy(kDeadline, std::nullopt), kDayMs, std::nullopt)
          .table_deadline_ms,
      kDeadline);
  EXPECT_EQ(
      ResolveCreationExpiration(false, -kDayMs, {}, 2 * kDayMs, std::nullopt).table_deadline_ms,
      kDayMs);
}

}  // namespace
}  // namespace bigquery_emulator_duckdb
