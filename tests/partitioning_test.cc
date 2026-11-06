#include "src/partitioning.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "src/api_error.h"
#include "src/backend.h"
#include "src/field_schema.h"

namespace bigquery_emulator_duckdb {
namespace {

int64_t Micros(int year, int month, int day, int hour = 0) {
  using namespace std::chrono;
  return duration_cast<microseconds>(
             (sys_days{std::chrono::year{year} / month / day} + hours{hour}).time_since_epoch())
      .count();
}

// Explicit evaluation times cannot be exercised through wall-clock-driven client APIs reliably.
// Check the predicate's results, rather than its generated SQL, at microsecond boundaries.
TEST(PartitioningTest, ExactBoundariesDoNotDependOnSessionTimezone) {
  Backend backend;
  const auto session = backend.NewSession();
  session->Execute("SET TimeZone = 'Asia/Tokyo'");
  struct Case {
    FieldType type;
    TimePartitionUnit unit;
    std::string value;
    int64_t deadline;
  };
  for (const auto& test : std::vector<Case>{
           {
               .type = FieldType::kDate,
               .unit = TimePartitionUnit::kDay,
               .value = "DATE '2024-02-29'",
               .deadline = Micros(2024, 3, 1),
           },
           {
               .type = FieldType::kDatetime,
               .unit = TimePartitionUnit::kDay,
               .value = "TIMESTAMP '2024-02-29 23:45:00'",
               .deadline = Micros(2024, 3, 1),
           },
           {
               .type = FieldType::kTimestamp,
               .unit = TimePartitionUnit::kDay,
               .value = "TIMESTAMPTZ '2024-03-01 08:45:00+09'",
               .deadline = Micros(2024, 3, 1),
           },
           {
               .type = FieldType::kDate,
               .unit = TimePartitionUnit::kMonth,
               .value = "DATE '2024-02-29'",
               .deadline = Micros(2024, 2, 2),
           },
           {
               .type = FieldType::kDatetime,
               .unit = TimePartitionUnit::kMonth,
               .value = "TIMESTAMP '2024-02-29 23:45:00'",
               .deadline = Micros(2024, 2, 2),
           },
           {
               .type = FieldType::kTimestamp,
               .unit = TimePartitionUnit::kMonth,
               .value = "TIMESTAMPTZ '2024-03-01 08:45:00+09'",
               .deadline = Micros(2024, 2, 2),
           },
           {
               .type = FieldType::kDate,
               .unit = TimePartitionUnit::kYear,
               .value = "DATE '2024-02-29'",
               .deadline = Micros(2024, 1, 2),
           },
           {
               .type = FieldType::kDatetime,
               .unit = TimePartitionUnit::kYear,
               .value = "TIMESTAMP '2024-02-29 23:45:00'",
               .deadline = Micros(2024, 1, 2),
           },
           {
               .type = FieldType::kTimestamp,
               .unit = TimePartitionUnit::kYear,
               .value = "TIMESTAMPTZ '2024-03-01 08:45:00+09'",
               .deadline = Micros(2024, 1, 2),
           },
           {
               .type = FieldType::kDatetime,
               .unit = TimePartitionUnit::kHour,
               .value = "TIMESTAMP '2024-02-29 23:45:00'",
               .deadline = Micros(2024, 3, 1, 23),
           },
           {
               .type = FieldType::kTimestamp,
               .unit = TimePartitionUnit::kHour,
               .value = "TIMESTAMPTZ '2024-03-01 08:45:00+09'",
               .deadline = Micros(2024, 3, 1, 23),
           },
       }) {
    SCOPED_TRACE(test.value + " " + std::string(TimePartitionUnitName(test.unit)));
    const std::vector<FieldSchema> schema{{.name = "D", .type = test.type}};
    const TimePartitioning partitioning{.type = test.unit, .field = "d"};
    for (const int64_t offset : {-1, 0, 1}) {
      const auto predicate =
          ExpiredPartitionPredicate(partitioning, schema, 86400000, test.deadline + offset);
      const auto result = session->Execute("SELECT CAST((" + predicate +
                                           ") AS BIGINT) FROM (SELECT " + test.value + " AS D)");
      ASSERT_EQ(result.rows.size(), 1);
      EXPECT_EQ(result.rows.Int64(0, 0), offset >= 0 ? 1 : 0);
    }
  }
}

TEST(PartitioningTest, SpecialPartitionsAndDurationLimits) {
  Backend backend;
  const std::vector<FieldSchema> schema{{.name = "d", .type = FieldType::kDate}};
  const TimePartitioning partitioning{.field = "d"};
  for (const auto& value : {
           "NULL::DATE",
           "DATE '0001-01-01'",
           "DATE '1959-12-31'",
           "DATE '2160-01-01'",
           "DATE '9999-12-31'",
       }) {
    const auto predicate =
        ExpiredPartitionPredicate(partitioning, schema, 1, std::numeric_limits<int64_t>::max());
    const auto result = backend.Execute("SELECT CAST((" + predicate + ") AS BIGINT) FROM (SELECT " +
                                        value + " AS d)");
    EXPECT_EQ(result.rows.Int64(0, 0), 0) << value;
  }
  for (const auto& value : {"DATE '1960-01-01'", "DATE '2159-12-31'"}) {
    for (const int64_t duration : {int64_t{1}, std::numeric_limits<int64_t>::max()}) {
      const auto predicate = ExpiredPartitionPredicate(partitioning, schema, duration,
                                                       std::numeric_limits<int64_t>::max());
      const auto result = backend.Execute("SELECT CAST((" + predicate +
                                          ") AS BIGINT) FROM (SELECT " + value + " AS d)");
      EXPECT_EQ(result.rows.Int64(0, 0), duration == 1 ? 1 : 0) << value;
    }
  }
  const auto predicate =
      ExpiredPartitionPredicate(partitioning, schema, 1, std::numeric_limits<int64_t>::min());
  EXPECT_EQ(backend
                .Execute("SELECT CAST((" + predicate +
                         ") AS BIGINT) FROM (SELECT DATE '1960-01-01' AS d)")
                .rows.Int64(0, 0),
            0);
  EXPECT_THROW((void)ExpiredPartitionPredicate(partitioning, schema, 0, 0), ApiError);
  EXPECT_THROW((void)ExpiredPartitionPredicate(partitioning, schema, -1, 0), ApiError);
}

}  // namespace
}  // namespace bigquery_emulator_duckdb
