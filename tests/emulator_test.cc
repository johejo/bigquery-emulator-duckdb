#include "src/emulator.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "nlohmann/json.hpp"
#include "src/field_schema.h"
#include "src/query_parameters.h"

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

  // The job's error message, or an empty string when the job succeeded.
  static std::string ErrorMessage(const Job& job) {
    return job.error.has_value() ? job.error->what() : "";
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
  EXPECT_EQ(Scalar("SELECT ARRAY_TO_STRING(['a', 'b', 'c'], ',')"), "a,b,c");
  EXPECT_EQ(Scalar("SELECT ARRAY_LENGTH(GENERATE_ARRAY(1, 5, 2))"), "3");
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

TEST_F(EmulatorTest, RunsResolvedByteLengthOverloads) {
  EXPECT_EQ(Scalar("SELECT BYTE_LENGTH('あ')"), "3");
  EXPECT_EQ(Scalar("SELECT BYTE_LENGTH(b'abc')"), "3");
  EXPECT_EQ(Scalar("SELECT BYTE_LENGTH(b'\\x00\\xff')"), "2");
  EXPECT_EQ(Scalar("SELECT BYTE_LENGTH('')"), "0");
  EXPECT_EQ(Scalar("SELECT BYTE_LENGTH(CAST(NULL AS STRING))"), std::nullopt);
  EXPECT_EQ(Scalar("SELECT BYTE_LENGTH(CAST(NULL AS BYTES))"), std::nullopt);
}

TEST_F(EmulatorTest, RunsAndPreparesResolvedParameters) {
  QueryRequest request;
  request.project_id = "test";
  request.parameters = QueryParameters::Parse(nlohmann::json::parse(R"([
    {"name":"s","parameterType":{"type":"STRING"},"parameterValue":{"value":"あ"}},
    {"name":"b","parameterType":{"type":"BYTES"},"parameterValue":{"value":"AP8="}}
  ])"));
  request.query = "SELECT BYTE_LENGTH(@s), BYTE_LENGTH(@b) AS bytes, SAFE_CAST(@s AS INT64)";
  for (const bool dry_run : {false, true}) {
    request.dry_run = dry_run;
    const auto job = emulator_.RunQuery(request);
    if (!job->result.has_value()) {
      FAIL() << ErrorMessage(*job);
    }
    const auto& result = job->result.value();
    ASSERT_EQ(result.schema.size(), 3);
    EXPECT_EQ(result.schema[0].name, "f0_");
    EXPECT_EQ(result.schema[1].name, "bytes");
    EXPECT_EQ(result.schema[2].name, "f1_");
    for (const auto& field : result.schema) {
      EXPECT_EQ(field.type, "INTEGER");
    }
    if (!dry_run) {
      ASSERT_EQ(result.rows.size(), 1);
      EXPECT_EQ(result.rows[0]["f"][0]["v"], "3");
      EXPECT_EQ(result.rows[0]["f"][1]["v"], "2");
      EXPECT_TRUE(result.rows[0]["f"][2]["v"].is_null());
    }
  }
  request.dry_run = false;
  request.query = "SELECT CAST(@s AS INT64)";
  EXPECT_TRUE(emulator_.RunQuery(request)->error.has_value());
}

TEST_F(EmulatorTest, RunsCallsWithTheSafePrefix) {
  EXPECT_EQ(Scalar("SELECT SAFE.SUBSTR('abcdef', 2, 3)"), "bcd");
  EXPECT_EQ(Scalar("SELECT SAFE.REGEXP_CONTAINS('abc', 'b')"), "true");
}

// The result schema follows BigQuery's typing and naming rather than DuckDB's.
TEST_F(EmulatorTest, ReportsTheResolvedResultSchema) {
  const std::shared_ptr<const Job> job =
      Run("SELECT SUM(x) AS s, ANY_VALUE('a'), 2.5, CURRENT_DATE() FROM (SELECT 1 AS x UNION ALL "
          "SELECT 2)");
  if (!job->result.has_value()) {
    FAIL() << ErrorMessage(*job);
  }
  const std::vector<FieldSchema>& schema = job->result->schema;
  ASSERT_EQ(schema.size(), 4);
  // DuckDB sums integers into a HUGEINT, which alone would be reported as BIGNUMERIC.
  EXPECT_EQ(schema[0].name, "s");
  EXPECT_EQ(schema[0].type, "INTEGER");
  EXPECT_EQ(schema[1].name, "f0_");
  EXPECT_EQ(schema[1].type, "STRING");
  EXPECT_EQ(schema[2].name, "f1_");
  EXPECT_EQ(schema[2].type, "FLOAT");
  EXPECT_EQ(schema[3].name, "f2_");
  EXPECT_EQ(schema[3].type, "DATE");
}

// FIXME: The translator keeps `UNNEST(...) AS x` as a table alias, so DuckDB binds `x` to the
// whole row, a STRUCT, instead of the element. This asserts the current failure so the bug is
// not forgotten; once the translator aliases the column, replace it with
// EXPECT_EQ(Scalar("SELECT SUM(x) FROM UNNEST([1, 2]) AS x"), "3");
TEST_F(EmulatorTest, FailsToAliasUnnestedElements) {
  EXPECT_EQ(ErrorStatus("SELECT SUM(x) FROM UNNEST([1, 2]) AS x"), 400);
}

TEST_F(EmulatorTest, AnalyzesQueriesAgainstTheTablesInDuckDb) {
  emulator_.CreateDataset({"test", "ds"});
  emulator_.CreateTable({"test", "ds", "t"}, nlohmann::json::parse(R"([
      {"name": "a", "type": "INTEGER"}, {"name": "b", "type": "STRING"}])"));
  QueryRequest request;
  request.project_id = "test";
  request.default_dataset = DatasetReference{"test", "ds"};
  request.query = "INSERT INTO t (a, b) VALUES (1, 'x'), (2, 'y')";
  ASSERT_FALSE(emulator_.RunQuery(request)->error.has_value());
  request.query = "SELECT SUM(a) FROM t";
  const std::shared_ptr<const Job> job = emulator_.RunQuery(request);
  if (!job->result.has_value()) {
    FAIL() << ErrorMessage(*job);
  }
  EXPECT_EQ(job->result->schema.at(0).name, "f0_");
  EXPECT_EQ(job->result->schema.at(0).type, "INTEGER");
  EXPECT_EQ(job->result->rows.at(0)["f"][0]["v"], "3");

  request.query = "SELECT nope FROM t";
  const std::shared_ptr<const Job> failed = emulator_.RunQuery(request);
  ASSERT_TRUE(failed->error.has_value());
  EXPECT_NE(ErrorMessage(*failed).find("Unrecognized name: nope"), std::string::npos)
      << ErrorMessage(*failed);
}

TEST_F(EmulatorTest, TypesQueryParameters) {
  QueryRequest request;
  request.project_id = "test";
  request.parameters = QueryParameters::Parse(nlohmann::json::parse(R"([
      {"name": "n", "parameterType": {"type": "INT64"}, "parameterValue": {"value": "2"}},
      {"name": "s", "parameterType": {"type": "STRING"}, "parameterValue": {"value": "2"}}])"));
  request.query = "SELECT @n * 2 AS x";
  const std::shared_ptr<const Job> job = emulator_.RunQuery(request);
  if (!job->result.has_value()) {
    FAIL() << ErrorMessage(*job);
  }
  EXPECT_EQ(job->result->schema.at(0).type, "INTEGER");
  EXPECT_EQ(job->result->rows.at(0)["f"][0]["v"], "4");

  // BigQuery does not coerce a STRING to a number, however DuckDB would.
  request.query = "SELECT @s * 2";
  EXPECT_TRUE(emulator_.RunQuery(request)->error.has_value());
  request.query = "SELECT @missing";
  EXPECT_TRUE(emulator_.RunQuery(request)->error.has_value());
}

// DDL is not analyzed, so a table the statement itself creates need not exist yet.
TEST_F(EmulatorTest, RunsDdlWithoutAnalysis) {
  emulator_.CreateDataset({"test", "ddl"});
  EXPECT_EQ(ErrorStatus("CREATE TABLE ddl.t (a INT64)"), 0);
  EXPECT_EQ(ErrorStatus("DROP TABLE ddl.t"), 0);
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
