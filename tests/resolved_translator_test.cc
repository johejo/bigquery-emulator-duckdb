#include "src/resolved_translator.h"

#include <exception>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "googlesql/public/type.h"
#include "gtest/gtest.h"
#include "nlohmann/json.hpp"
#include "src/analyzer.h"
#include "src/backend.h"
#include "src/catalog.h"
#include "src/frontend.h"

namespace bigquery_emulator_duckdb {
namespace {

class TestTableSource : public TableSource {
 public:
  std::optional<std::vector<FieldSchema>> FindTable(const std::string& project,
                                                    const std::string& dataset,
                                                    const std::string& table) override {
    if (project != "p" || dataset != "ds" || table != "t") {
      return std::nullopt;
    }
    return std::vector<FieldSchema>{{.name = "a", .type = "INTEGER"},
                                    {.name = "b", .type = "STRING"},
                                    {.name = "raw", .type = "BYTES"}};
  }
};

class ResolvedTranslatorTest : public ::testing::Test {
 protected:
  std::optional<std::string> Translate(const std::string& sql,
                                       const QueryParameters& parameters = {},
                                       const AnalyzerSettings& settings = {}) {
    const auto analyzed = AnalyzeGoogleSql(ParseGoogleSql(sql), catalog_, types_, settings);
    return TranslateResolvedToDuckDbSql(analyzed.statement(), parameters);
  }

  QueryResult Execute(const std::string& sql, const QueryParameters& parameters = {},
                      const AnalyzerSettings& settings = {}) {
    const auto translated = Translate(sql, parameters, settings);
    if (!translated) {
      throw std::runtime_error("Unexpected parser fallback: " + sql);
    }
    return backend_.Execute(*translated);
  }

  std::optional<std::string> Scalar(const std::string& sql) {
    const auto result = Execute(sql);
    if (result.rows.size() != 1 || result.schema.size() != 1) {
      throw std::runtime_error("Expected one cell: " + sql);
    }
    const auto& cell = result.rows[0]["f"][0]["v"];
    return cell.is_null() ? std::nullopt : std::optional<std::string>(cell.get<std::string>());
  }

  void SetUp() override {
    backend_.Execute("ATTACH ':memory:' AS p");
    backend_.Execute("CREATE SCHEMA p.ds");
    backend_.Execute("CREATE TABLE p.ds.t (a BIGINT, b VARCHAR, raw BLOB)");
    backend_.Execute(
        "INSERT INTO p.ds.t VALUES (3, 'あ', from_hex('00ff')), "
        "(1, 'x', from_hex('61')), (2, 'yy', NULL), (NULL, NULL, NULL)");
  }

  Backend backend_;
  googlesql::TypeFactory types_;
  TestTableSource source_;
  BigQueryCatalog catalog_{source_, &types_, "p", "ds"};
};

TEST_F(ResolvedTranslatorTest, SelectsByteLengthOverloadFromResolvedType) {
  const auto sql = Translate("SELECT BYTE_LENGTH('あ') AS s, BYTE_LENGTH(b'abc') AS b");
  if (!sql.has_value()) {
    FAIL() << "Expected resolved translation";
  }
  EXPECT_NE(sql.value().find("strlen("), std::string::npos);
  EXPECT_NE(sql.value().find("octet_length("), std::string::npos);
}

TEST_F(ResolvedTranslatorTest, PreservesScalarTypesAndOutputOrder) {
  const auto sql = Translate("SELECT 1 AS z, 2.5 AS a, NULL AS n, TRUE AS b, b'\\x00\\xff' AS raw");
  if (!sql.has_value()) {
    FAIL() << "Expected resolved translation";
  }
  const auto result = backend_.Execute(*sql);
  ASSERT_EQ(result.schema.size(), 5);
  ASSERT_EQ(result.rows.size(), 1);
  EXPECT_EQ(result.schema[0].name, "z");
  EXPECT_EQ(result.schema[1].name, "a");
  EXPECT_EQ(result.schema[0].type, "INTEGER");
  EXPECT_EQ(result.schema[1].type, "FLOAT");
  EXPECT_EQ(result.rows[0]["f"][0]["v"], "1");
  EXPECT_EQ(result.rows[0]["f"][1]["v"], "2.5");
  EXPECT_TRUE(result.rows[0]["f"][2]["v"].is_null());
  EXPECT_EQ(result.rows[0]["f"][3]["v"], "true");
  EXPECT_EQ(result.rows[0]["f"][4]["v"], "AP8=");
}

TEST_F(ResolvedTranslatorTest, SubstitutesParametersAndSafeCasts) {
  const auto parameters = QueryParameters::Parse(nlohmann::json::parse(R"([
    {"name":"s","parameterType":{"type":"STRING"},"parameterValue":{"value":"あ"}}
  ])"));
  AnalyzerSettings settings;
  settings.named_parameters.emplace_back("s", googlesql::types::StringType());
  const auto sql =
      Translate("SELECT BYTE_LENGTH(@s), SAFE_CAST(@s AS INT64)", parameters, settings);
  if (!sql.has_value()) {
    FAIL() << "Expected resolved translation";
  }
  EXPECT_NE(sql.value().find("strlen("), std::string::npos);
  EXPECT_NE(sql.value().find("TRY_CAST("), std::string::npos);

  settings.named_parameters.clear();
  settings.positional_parameters.push_back(googlesql::types::StringType());
  const auto positional = QueryParameters::Parse(nlohmann::json::parse(R"([
    {"parameterType":{"type":"STRING"},"parameterValue":{"value":"abc"}}
  ])"));
  EXPECT_TRUE(Translate("SELECT BYTE_LENGTH(?)", positional, settings).has_value());
}

TEST_F(ResolvedTranslatorTest, MatchesDuplicateAliasesByColumnId) {
  const auto sql = Translate("SELECT 1 AS same, 2 AS same, 3 AS `a.b`");
  if (!sql.has_value()) {
    FAIL() << "Expected resolved translation";
  }
  const auto result = backend_.Execute(*sql);
  ASSERT_EQ(result.schema.size(), 3);
  ASSERT_EQ(result.rows.size(), 1);
  EXPECT_EQ(result.schema[0].name, "same");
  EXPECT_EQ(result.schema[1].name, "same");
  EXPECT_EQ(result.schema[2].name, "a.b");
  for (size_t i = 0; i < 3; ++i) {
    EXPECT_EQ(result.rows[0]["f"][i]["v"], std::to_string(i + 1));
  }
}

TEST_F(ResolvedTranslatorTest, FallsBackForUnsupportedConstructs) {
  for (const auto* sql :
       {"SELECT STRUCT(1 AS x)", "SELECT x FROM UNNEST([1, 2]) AS x", "SELECT SUM(a) FROM t",
        "SELECT DISTINCT a FROM t", "SELECT l.a FROM t AS l JOIN t AS r ON l.a = r.a",
        "WITH c AS (SELECT 1 AS x) SELECT x FROM c", "SELECT 1 UNION ALL SELECT 2",
        "SELECT a FROM t QUALIFY ROW_NUMBER() OVER (ORDER BY a) = 1", "SELECT AS VALUE 1",
        "SELECT SAFE.BYTE_LENGTH('abc')", "SELECT BYTE_LENGTH('abc'), SESSION_USER()",
        "CREATE TABLE ds.new_t (x INT64)"}) {
    EXPECT_FALSE(Translate(sql).has_value()) << sql;
  }
}

TEST_F(ResolvedTranslatorTest, ReadsTablesAndKeepsHiddenSortColumns) {
  const auto result = Execute(
      "SELECT BYTE_LENGTH(b) AS bytes, BYTE_LENGTH(raw) AS raw_bytes FROM `p.ds.t` "
      "WHERE a >= 1 AND a < 4 ORDER BY a DESC LIMIT 2 OFFSET 1");
  ASSERT_EQ(result.rows.size(), 2);
  EXPECT_EQ(result.rows[0]["f"][0]["v"], "2");
  EXPECT_TRUE(result.rows[0]["f"][1]["v"].is_null());
  EXPECT_EQ(result.rows[1]["f"][0]["v"], "1");
  EXPECT_EQ(result.rows[1]["f"][1]["v"], "1");
  ASSERT_EQ(result.schema.size(), 2);
  EXPECT_EQ(result.schema[0].name, "bytes");
  EXPECT_EQ(result.schema[0].type, "INTEGER");
}

TEST_F(ResolvedTranslatorTest, ResolvesScopesAliasesAndStarModifiers) {
  const auto result = Execute(
      "SELECT q.a AS same, q.b AS same, q.a + 10 AS `_c1` "
      "FROM (SELECT a + 1 AS a, b FROM t WHERE a IS NOT NULL) AS q "
      "WHERE q.a > 2 ORDER BY q.a DESC");
  ASSERT_EQ(result.rows.size(), 2);
  ASSERT_EQ(result.schema.size(), 3);
  EXPECT_EQ(result.schema[0].name, "same");
  EXPECT_EQ(result.schema[1].name, "same");
  EXPECT_EQ(result.schema[2].name, "_c1");
  EXPECT_EQ(result.rows[0]["f"][0]["v"], "4");
  EXPECT_EQ(result.rows[0]["f"][1]["v"], "あ");
  EXPECT_EQ(result.rows[0]["f"][2]["v"], "14");
  EXPECT_EQ(result.rows[1]["f"][0]["v"], "3");
  const auto star = Execute(
      "SELECT * EXCEPT(raw) REPLACE(a * 10 AS a) FROM t "
      "WHERE a IS NOT NULL ORDER BY a DESC LIMIT 1");
  ASSERT_EQ(star.schema.size(), 2);
  ASSERT_EQ(star.rows.size(), 1);
  EXPECT_EQ(star.rows[0]["f"][0]["v"], "30");
  EXPECT_EQ(star.rows[0]["f"][1]["v"], "あ");
}

TEST_F(ResolvedTranslatorTest, PreservesCardinalityWithoutReferencedTableColumns) {
  EXPECT_EQ(Execute("SELECT 42 FROM t").rows.size(), 4);
  EXPECT_TRUE(Execute("SELECT 42 FROM t WHERE FALSE").rows.empty());
  EXPECT_TRUE(Execute("SELECT a FROM t LIMIT 0").rows.empty());
  EXPECT_EQ(Execute("SELECT 42 FROM t LIMIT 2").rows.size(), 2);
}

TEST_F(ResolvedTranslatorTest, SortsNullsAndAppliesLimitsAtTheCorrectScope) {
  for (const auto* order : {"a", "a ASC NULLS FIRST", "a DESC NULLS FIRST"}) {
    const auto result = Execute(std::string("SELECT a FROM t ORDER BY ") + order + " LIMIT 1");
    ASSERT_EQ(result.rows.size(), 1);
    EXPECT_TRUE(result.rows[0]["f"][0]["v"].is_null()) << order;
  }
  for (const auto* order : {"a DESC", "a DESC NULLS LAST", "a ASC NULLS LAST"}) {
    const auto result = Execute(std::string("SELECT a FROM t ORDER BY ") + order + " LIMIT 1");
    ASSERT_EQ(result.rows.size(), 1);
    EXPECT_FALSE(result.rows[0]["f"][0]["v"].is_null()) << order;
  }
  const auto result = Execute(
      "SELECT a FROM (SELECT a FROM t ORDER BY a DESC LIMIT 2) "
      "ORDER BY a LIMIT 1");
  ASSERT_EQ(result.rows.size(), 1);
  EXPECT_EQ(result.rows[0]["f"][0]["v"], "2");
  const auto computed = Execute("SELECT a FROM t WHERE a IS NOT NULL ORDER BY -a LIMIT 1");
  EXPECT_EQ(computed.rows.at(0)["f"][0]["v"], "3");
  const auto ordinal = Execute("SELECT -a AS a FROM t WHERE a IS NOT NULL ORDER BY 1 LIMIT 1");
  EXPECT_EQ(ordinal.rows.at(0)["f"][0]["v"], "-3");
}

TEST_F(ResolvedTranslatorTest, RunsOperatorsAndConditionsWithoutFallback) {
  const auto result = Execute(
      "SELECT (1 + 2) * 3 - 4, 3 / 2, NOT (1 > 2 OR 2 <> 2), "
      "2 BETWEEN 1 AND 3, 2 IN (1, 2, NULL), 'abc' LIKE 'a%', "
      "CASE WHEN a IS NULL THEN 'null' WHEN a = 1 THEN 'one' ELSE 'other' END, "
      "CASE a WHEN 1 THEN 10 WHEN 2 THEN 20 ELSE 0 END, "
      "IF(a IS NULL, 0, a), COALESCE(a, 5), IFNULL(a, 6), NULLIF(a, 1) "
      "FROM t ORDER BY a");
  ASSERT_EQ(result.rows.size(), 4);
  const auto& row = result.rows[0]["f"];
  EXPECT_EQ(row[0]["v"], "5");
  EXPECT_EQ(row[1]["v"], "1.5");
  for (int i = 2; i < 6; ++i) {
    EXPECT_EQ(row[i]["v"], "true");
  }
  EXPECT_EQ(row[6]["v"], "null");
  EXPECT_EQ(row[7]["v"], "0");
  EXPECT_EQ(row[8]["v"], "0");
  EXPECT_EQ(row[9]["v"], "5");
  EXPECT_EQ(row[10]["v"], "6");
  EXPECT_TRUE(row[11]["v"].is_null());
  EXPECT_EQ(result.rows[1]["f"][6]["v"], "one");
  EXPECT_EQ(result.rows[1]["f"][7]["v"], "10");
  EXPECT_TRUE(result.rows[1]["f"][11]["v"].is_null());
  EXPECT_THROW(Execute("SELECT 1 / 0"), BackendError);
  EXPECT_EQ(Scalar("SELECT CAST(NULL AS FLOAT64) / 0"), std::nullopt);
  EXPECT_EQ(Scalar("SELECT 1 / CAST(NULL AS FLOAT64)"), std::nullopt);
  const auto division = Translate("SELECT 1 / RAND()");
  if (!division) {
    FAIL() << "Expected resolved translation";
  }
  const auto random = division->find("random()");
  ASSERT_NE(random, std::string::npos);
  EXPECT_EQ(division->find("random()", random + 1), std::string::npos);
  EXPECT_EQ(Execute("SELECT IF(a = 1, 42, 1 / (a - 1)) FROM t WHERE a = 1").rows.at(0)["f"][0]["v"],
            "42.0");
  EXPECT_EQ(Execute("SELECT IF(FALSE, 1 / 0, 42)").rows.at(0)["f"][0]["v"], "42.0");
}

TEST_F(ResolvedTranslatorTest, PreparesParameterizedTableQueriesWithoutFallback) {
  const auto parameters = QueryParameters::Parse(nlohmann::json::parse(R"([
    {"name":"min", "parameterType":{"type":"INT64"}, "parameterValue":{"value":"1"}},
    {"name":"n", "parameterType":{"type":"INT64"}, "parameterValue":{"value":"1"}},
    {"name":"skip", "parameterType":{"type":"INT64"}, "parameterValue":{"value":"1"}}
  ])"));
  AnalyzerSettings settings;
  for (const auto* name : {"min", "n", "skip"}) {
    settings.named_parameters.emplace_back(name, googlesql::types::Int64Type());
  }
  const auto sql = Translate(
      "SELECT a + @min AS x FROM t WHERE a > @min "
      "ORDER BY a DESC LIMIT @n OFFSET @skip",
      parameters, settings);
  if (!sql.has_value()) {
    FAIL() << "Expected resolved translation";
  }
  const auto prepared = backend_.Prepare(*sql);
  ASSERT_EQ(prepared.schema.size(), 1);
  EXPECT_EQ(prepared.schema[0].name, "x");
  EXPECT_EQ(prepared.schema[0].type, "INTEGER");
  const auto result = backend_.Execute(*sql);
  ASSERT_EQ(result.rows.size(), 1);
  EXPECT_EQ(result.rows[0]["f"][0]["v"], "3");
}

TEST_F(ResolvedTranslatorTest, RunsRenamedFunctions) {
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

TEST_F(ResolvedTranslatorTest, RunsDateAndTimeArithmetic) {
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

TEST_F(ResolvedTranslatorTest, RunsDatePartFunctions) {
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

TEST_F(ResolvedTranslatorTest, RunsFormattingAndParsing) {
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

TEST_F(ResolvedTranslatorTest, RunsEpochConversions) {
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

TEST_F(ResolvedTranslatorTest, RunsCallsWithDifferentSemantics) {
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

TEST_F(ResolvedTranslatorTest, DoesNotConvertParameterErrorsToFallback) {
  AnalyzerSettings settings;
  settings.named_parameters.emplace_back("s", googlesql::types::StringType());
  EXPECT_THROW(Translate("SELECT BYTE_LENGTH(@s)", {}, settings), std::exception);
}

}  // namespace
}  // namespace bigquery_emulator_duckdb
