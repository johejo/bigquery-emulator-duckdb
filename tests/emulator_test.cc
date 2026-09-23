#include "src/emulator.h"

#include <memory>
#include <optional>
#include <string>

#include "gtest/gtest.h"
#include "nlohmann/json.hpp"

namespace bigquery_emulator_duckdb {
namespace {

// The translator tests check what a query is translated into; these check that the translation
// runs on DuckDB and produces the value BigQuery would. Every expectation is BigQuery's answer,
// not DuckDB's: the two differ for several of the functions below, which is the point.
class EmulatorTest : public ::testing::Test {
 protected:
  std::shared_ptr<const Job> Run(const std::string& sql) {
    QueryRequest request;
    request.project_id = "test";
    request.query = sql;
    return emulator_.RunQuery(request);
  }

  // The HTTP status the job's error would be reported with, or 0 when the query succeeded.
  int ErrorStatus(const std::string& sql) {
    const std::shared_ptr<const Job> job = Run(sql);
    return job->error.has_value() ? job->error->http_status() : 0;
  }

  // Runs `sql` and returns the only cell of its only row, or nullopt when that cell is NULL.
  std::optional<std::string> Scalar(const std::string& sql) {
    const std::shared_ptr<const Job> job = Run(sql);
    if (job->error.has_value()) {
      ADD_FAILURE() << sql << "\n  " << job->error->what();
      return std::nullopt;
    }
    if (!job->result.has_value()) {
      ADD_FAILURE() << sql << "\n  the job has neither a result nor an error";
      return std::nullopt;
    }
    const QueryResult& result = *job->result;
    EXPECT_EQ(result.schema.size(), 1) << sql;
    EXPECT_EQ(result.rows.size(), 1) << sql;
    if (result.schema.size() != 1 || result.rows.size() != 1) {
      return std::nullopt;
    }
    const nlohmann::json& value = result.rows[0]["f"][0]["v"];
    if (value.is_null()) {
      return std::nullopt;
    }
    return value.get<std::string>();
  }

  Emulator emulator_;
};

TEST_F(EmulatorTest, RunsRenamedFunctions) {
  EXPECT_EQ(Scalar("SELECT REGEXP_CONTAINS('abc', 'b')"), "true");
  EXPECT_EQ(Scalar("SELECT CONTAINS_SUBSTR('abcdef', 'cd')"), "true");
  EXPECT_EQ(Scalar("SELECT DIV(7, 2)"), "3");
  EXPECT_EQ(Scalar("SELECT FORMAT('%s-%d', 'x', 3)"), "x-3");
  EXPECT_EQ(Scalar("SELECT ARRAY_TO_STRING(GENERATE_ARRAY(1, 5, 2), ',')"), "1,3,5");
  // DuckDB's uuid() is typed UUID rather than a string, which the cast makes explicit.
  EXPECT_EQ(Scalar("SELECT LENGTH(CAST(GENERATE_UUID() AS STRING))"), "36");
  EXPECT_EQ(Scalar("SELECT RAND() >= 0 AND RAND() < 1"), "true");
  EXPECT_EQ(Scalar("SELECT IS_INF(CAST('inf' AS FLOAT64))"), "true");
  EXPECT_EQ(Scalar("SELECT IS_NAN(CAST('nan' AS FLOAT64))"), "true");
  EXPECT_EQ(Scalar(R"(SELECT JSON_EXTRACT_SCALAR('{"a": 1}', '$.a'))"), "1");
  EXPECT_EQ(Scalar(R"(SELECT JSON_QUERY('{"a": {"b": 1}}', '$.a'))"), R"({"b":1})");
  EXPECT_EQ(Scalar(R"(SELECT TO_JSON_STRING(PARSE_JSON('{"a":1}')))"), R"({"a":1})");
}

// LOGICAL_AND and LOGICAL_OR are aggregates, so they also cover a rename that has to survive
// being folded into a GROUP BY.
TEST_F(EmulatorTest, RunsRenamedAggregates) {
  EXPECT_EQ(Scalar("SELECT LOGICAL_AND(a) FROM (SELECT true AS a UNION ALL SELECT false)"),
            "false");
  EXPECT_EQ(Scalar("SELECT LOGICAL_OR(a) FROM (SELECT true AS a UNION ALL SELECT false)"), "true");
  EXPECT_EQ(Scalar("SELECT COUNTIF(a > 1) FROM (SELECT 1 AS a UNION ALL SELECT 2)"), "1");
}

TEST_F(EmulatorTest, RunsDateAndTimeArithmetic) {
  EXPECT_EQ(Scalar("SELECT DATE_ADD(DATE '2024-01-31', INTERVAL 1 MONTH)"), "2024-02-29");
  EXPECT_EQ(Scalar("SELECT DATE_SUB(DATE '2024-03-01', INTERVAL 1 DAY)"), "2024-02-29");
  EXPECT_EQ(Scalar("SELECT FORMAT_DATETIME('%Y-%m-%d %H:%M:%S',"
                   "                       DATETIME_ADD(DATETIME '2024-01-01 00:00:00',"
                   "                                    INTERVAL 90 MINUTE))"),
            "2024-01-01 01:30:00");
  EXPECT_EQ(Scalar("SELECT FORMAT_TIMESTAMP('%Y-%m-%d %H:%M:%S',"
                   "                        TIMESTAMP_SUB(TIMESTAMP '2024-01-01 00:00:00',"
                   "                                      INTERVAL 1 HOUR))"),
            "2023-12-31 23:00:00");
  EXPECT_EQ(Scalar("SELECT CAST(TIME_ADD(TIME '10:00:00', INTERVAL 1 HOUR) AS STRING)"),
            "11:00:00");
  EXPECT_EQ(Scalar("SELECT CAST(TIME_SUB(TIME '10:00:00', INTERVAL 30 MINUTE) AS STRING)"),
            "09:30:00");
  EXPECT_EQ(Scalar("SELECT FORMAT_DATETIME('%Y-%m-%d %H:%M:%S',"
                   "                       DATETIME_SUB(DATETIME '2024-01-01 00:00:00',"
                   "                                    INTERVAL 1 SECOND))"),
            "2023-12-31 23:59:59");
  EXPECT_EQ(Scalar("SELECT FORMAT_TIMESTAMP('%Y-%m-%d %H:%M:%S',"
                   "                        TIMESTAMP_ADD(TIMESTAMP '2024-01-01 00:00:00',"
                   "                                      INTERVAL 90 MINUTE))"),
            "2024-01-01 01:30:00");
}

// The date part is a keyword in BigQuery and a string in DuckDB, and DIFF takes its arguments
// the other way round, so a mix-up would show up as a sign or a wrong unit rather than an error.
TEST_F(EmulatorTest, RunsDatePartFunctions) {
  EXPECT_EQ(Scalar("SELECT DATE_DIFF(DATE '2024-03-01', DATE '2024-01-01', DAY)"), "60");
  EXPECT_EQ(Scalar("SELECT DATE_DIFF(DATE '2024-01-01', DATE '2024-03-01', MONTH)"), "-2");
  EXPECT_EQ(Scalar("SELECT TIMESTAMP_DIFF(TIMESTAMP '2024-01-02 00:00:00',"
                   "                      TIMESTAMP '2024-01-01 00:00:00', HOUR)"),
            "24");
  EXPECT_EQ(Scalar("SELECT DATETIME_DIFF(DATETIME '2024-01-01 00:01:00',"
                   "                     DATETIME '2024-01-01 00:00:00', SECOND)"),
            "60");
  EXPECT_EQ(Scalar("SELECT TIME_DIFF(TIME '11:00:00', TIME '10:00:00', MINUTE)"), "60");
  EXPECT_EQ(Scalar("SELECT DATE_TRUNC(DATE '2024-05-17', MONTH)"), "2024-05-01");
  EXPECT_EQ(Scalar("SELECT FORMAT_DATETIME('%Y-%m-%d %H:%M:%S',"
                   "                       DATETIME_TRUNC(DATETIME '2024-05-17 10:20:30', HOUR))"),
            "2024-05-17 10:00:00");
  EXPECT_EQ(
      Scalar("SELECT FORMAT_TIMESTAMP('%Y-%m-%d %H:%M:%S',"
             "                        TIMESTAMP_TRUNC(TIMESTAMP '2024-05-17 10:20:30', DAY))"),
      "2024-05-17 00:00:00");
}

TEST_F(EmulatorTest, RunsFormattingAndParsing) {
  EXPECT_EQ(Scalar("SELECT FORMAT_DATE('%Y/%m/%d', DATE '2024-01-02')"), "2024/01/02");
  EXPECT_EQ(Scalar("SELECT FORMAT_TIMESTAMP('%Y-%m-%dT%H:%M:%SZ',"
                   "                        TIMESTAMP '2024-01-02 03:04:05')"),
            "2024-01-02T03:04:05Z");
  EXPECT_EQ(Scalar("SELECT PARSE_DATE('%Y-%m-%d', '2024-02-29')"), "2024-02-29");
  EXPECT_EQ(Scalar("SELECT FORMAT_DATETIME('%Y-%m-%d %H:%M:%S',"
                   "                       PARSE_DATETIME('%Y-%m-%d %H:%M:%S',"
                   "                                      '2024-01-02 03:04:05'))"),
            "2024-01-02 03:04:05");
  EXPECT_EQ(Scalar("SELECT FORMAT_TIMESTAMP('%Y-%m-%d %H:%M:%S',"
                   "                        PARSE_TIMESTAMP('%Y-%m-%d %H:%M:%S',"
                   "                                        '2024-01-02 03:04:05'))"),
            "2024-01-02 03:04:05");
}

// The epoch functions have to land on the instant BigQuery means, which is where reading the
// civil timestamp DuckDB returns as UTC matters.
TEST_F(EmulatorTest, RunsEpochConversions) {
  EXPECT_EQ(Scalar("SELECT UNIX_SECONDS(TIMESTAMP '2020-01-01 00:00:00')"), "1577836800");
  EXPECT_EQ(Scalar("SELECT UNIX_MILLIS(TIMESTAMP '2020-01-01 00:00:00')"), "1577836800000");
  EXPECT_EQ(Scalar("SELECT UNIX_MICROS(TIMESTAMP '2020-01-01 00:00:00')"), "1577836800000000");
  EXPECT_EQ(Scalar("SELECT FORMAT_TIMESTAMP('%Y-%m-%d %H:%M:%S', TIMESTAMP_SECONDS(1577836800))"),
            "2020-01-01 00:00:00");
  EXPECT_EQ(Scalar("SELECT FORMAT_TIMESTAMP('%Y-%m-%d %H:%M:%S', TIMESTAMP_MILLIS(1577836800000))"),
            "2020-01-01 00:00:00");
  EXPECT_EQ(
      Scalar("SELECT FORMAT_TIMESTAMP('%Y-%m-%d %H:%M:%S', TIMESTAMP_MICROS(1577836800000000))"),
      "2020-01-01 00:00:00");
  EXPECT_EQ(Scalar("SELECT DATE_FROM_UNIX_DATE(18262)"), "2020-01-01");
}

// These are the cases where DuckDB answers a different question, so running them is the only
// way to tell the translation apart from a plausible looking one.
TEST_F(EmulatorTest, RunsCallsWithDifferentSemantics) {
  // DuckDB would return +Inf rather than NULL.
  EXPECT_EQ(Scalar("SELECT SAFE_DIVIDE(1, 0)"), std::nullopt);
  EXPECT_EQ(Scalar("SELECT SAFE_DIVIDE(3, 2)"), "1.5");
  // DuckDB's log() is the base 10 logarithm, so an untranslated LOG(100) would answer 2.
  EXPECT_EQ(Scalar("SELECT CAST(ROUND(LOG(EXP(1))) AS INT64)"), "1");
  EXPECT_EQ(Scalar("SELECT CAST(LOG(8, 2) AS INT64)"), "3");
  // DuckDB replaces only the first occurrence without the global flag.
  EXPECT_EQ(Scalar("SELECT REGEXP_REPLACE('aaa', 'a', 'b')"), "bbb");
  EXPECT_EQ(Scalar("SELECT ARRAY_TO_STRING(SPLIT('a,b'), '|')"), "a|b");
}

TEST_F(EmulatorTest, RunsCallsWithTheSafePrefix) {
  EXPECT_EQ(Scalar("SELECT SAFE.SUBSTR('abcdef', 2, 3)"), "bcd");
  EXPECT_EQ(Scalar("SELECT SAFE.REGEXP_CONTAINS('abc', 'b')"), "true");
}

// A query that fails is reported through the job rather than thrown, which is how BigQuery
// reports it too.
TEST_F(EmulatorTest, ReportsAFailedQueryAsAJobError) {
  EXPECT_EQ(ErrorStatus("SELECT * FROM missing_table"), 400);
  EXPECT_EQ(ErrorStatus("SELECT FROM WHERE"), 400);
  EXPECT_EQ(ErrorStatus("SELECT 1"), 0);
}

}  // namespace
}  // namespace bigquery_emulator_duckdb
