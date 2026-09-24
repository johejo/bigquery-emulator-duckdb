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

  std::string Unsupported(const std::string& sql) {
    const auto analyzed = AnalyzeGoogleSql(ParseGoogleSql(sql), catalog_, types_, {});
    std::string reason;
    if (TranslateResolvedToDuckDbSql(analyzed.statement(), {}, &reason).has_value()) {
      throw std::runtime_error("Unexpected resolved translation: " + sql);
    }
    return reason;
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
  const std::string recursive =
      "WITH RECURSIVE c AS (SELECT 1 AS n UNION ALL SELECT n + 1 FROM c WHERE n < 3) "
      "SELECT n FROM c";
  for (const std::string& sql :
       {std::string("SELECT AS VALUE 1"), std::string("SELECT SAFE.RAND()"),
        std::string("SELECT BYTE_LENGTH('abc'), SESSION_USER()"),
        std::string("CREATE TABLE ds.new_t (x INT64)"), recursive,
        std::string("SELECT a, COUNT(*) FROM t GROUP BY ROLLUP(a)"),
        std::string("SELECT STRUCT(1, 2)")}) {
    EXPECT_FALSE(Translate(sql).has_value()) << sql;
  }
}

TEST_F(ResolvedTranslatorTest, NamesTheUnsupportedConstruct) {
  EXPECT_EQ(Unsupported("SELECT BYTE_LENGTH('abc'), SESSION_USER()"), "function SESSION_USER");
  EXPECT_EQ(Unsupported("SELECT a FROM t WHERE a IN (SELECT SAFE.RAND() FROM t)"), "SAFE.RAND");
  EXPECT_EQ(Unsupported("SELECT a, COUNT(*) FROM t GROUP BY ROLLUP(a)"),
            "GROUPING SETS, ROLLUP, CUBE or GROUPING");
  EXPECT_EQ(Unsupported("SELECT AS VALUE a FROM t"), "SELECT AS STRUCT or AS VALUE");
  EXPECT_EQ(Unsupported("UPDATE t SET a = 1 WHERE TRUE ASSERT_ROWS_MODIFIED 1"),
            "UPDATE with ASSERT_ROWS_MODIFIED, THEN RETURN or generated columns");
}

TEST_F(ResolvedTranslatorTest, RunsSafeCalls) {
  EXPECT_EQ(Scalar("SELECT SAFE.LENGTH('abc')"), "3");
  EXPECT_EQ(Scalar("SELECT SAFE.REGEXP_CONTAINS('abc', '(')"), std::nullopt);
  EXPECT_EQ(Scalar("SELECT SAFE.REGEXP_CONTAINS(b, '(') FROM t WHERE a = 1"), std::nullopt);
  EXPECT_EQ(Scalar("SELECT SAFE.REGEXP_CONTAINS(b, 'x') FROM t WHERE a = 1"), "true");
  EXPECT_EQ(Scalar("SELECT SAFE.CONCAT(SAFE.UPPER(b), SAFE.LOWER('Z')) FROM t WHERE a = 1"), "Xz");
  EXPECT_EQ(Scalar("SELECT SAFE.DATE_TRUNC(DATE '2024-05-06', MONTH)"), "2024-05-01");
  // Only the function's own errors become NULL, not those of its arguments.
  EXPECT_THROW(Execute("SELECT SAFE.ABS(1 / 0)"), BackendError);
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

std::vector<std::string> Column(const QueryResult& result, size_t index = 0) {
  std::vector<std::string> values;
  for (const auto& row : result.rows) {
    const auto& cell = row["f"][index]["v"];
    values.push_back(cell.is_null() ? "NULL" : cell.get<std::string>());
  }
  return values;
}

TEST_F(ResolvedTranslatorTest, RunsJoinsAndCtes) {
  using V = std::vector<std::string>;
  EXPECT_EQ(Column(Execute("SELECT l.a + r.a FROM t AS l JOIN t AS r ON l.a = r.a ORDER BY 1")),
            (V{"2", "4", "6"}));
  EXPECT_EQ(Column(Execute("SELECT r.b FROM t AS l LEFT JOIN t AS r ON l.a = r.a + 1 "
                           "ORDER BY l.a NULLS LAST")),
            (V{"NULL", "x", "yy", "NULL"}));
  EXPECT_EQ(Execute("SELECT 1 FROM t AS l CROSS JOIN t AS r").rows.size(), 16);
  EXPECT_EQ(Execute("SELECT 1 FROM t AS l FULL JOIN t AS r ON l.a = r.a").rows.size(), 5);
  EXPECT_EQ(Column(Execute("SELECT a FROM t JOIN (SELECT 2 AS a) USING (a)")), (V{"2"}));
  EXPECT_EQ(Column(Execute("WITH c AS (SELECT a * 10 AS x FROM t), d AS (SELECT x FROM c "
                           "WHERE x > 10) SELECT x FROM d ORDER BY x DESC")),
            (V{"30", "20"}));
  EXPECT_EQ(Column(Execute("WITH c AS (SELECT 1 AS x) SELECT l.x + r.x FROM c AS l, c AS r")),
            (V{"2"}));
}

TEST_F(ResolvedTranslatorTest, RunsAggregatesAndDistinct) {
  using V = std::vector<std::string>;
  const auto result = Execute(
      "SELECT SUM(a), COUNT(*), COUNT(a), COUNT(DISTINCT a), AVG(a), MIN(b), MAX(b), "
      "STRING_AGG(b, ',' ORDER BY a DESC), ARRAY_LENGTH(ARRAY_AGG(a IGNORE NULLS)), "
      "COUNTIF(a > 1), LOGICAL_AND(a > 0) FROM t");
  ASSERT_EQ(result.rows.size(), 1);
  EXPECT_EQ(result.schema[0].type, "INTEGER");
  const auto& row = result.rows[0]["f"];
  EXPECT_EQ(row[0]["v"], "6");
  EXPECT_EQ(row[1]["v"], "4");
  EXPECT_EQ(row[2]["v"], "3");
  EXPECT_EQ(row[3]["v"], "3");
  EXPECT_EQ(row[4]["v"], "2.0");
  EXPECT_EQ(row[5]["v"], "x");
  EXPECT_EQ(row[6]["v"], "あ");
  EXPECT_EQ(row[7]["v"], "あ,yy,x");
  EXPECT_EQ(row[8]["v"], "3");
  EXPECT_EQ(row[9]["v"], "2");
  EXPECT_EQ(row[10]["v"], "true");
  EXPECT_EQ(Column(Execute("SELECT MOD(a, 2) AS k, COUNT(*) AS n FROM t WHERE a IS NOT NULL "
                           "GROUP BY k HAVING COUNT(*) > 1")),
            (V{"1"}));
  EXPECT_EQ(Column(Execute("SELECT DISTINCT a > 1 AS big FROM t ORDER BY big")),
            (V{"NULL", "false", "true"}));
  EXPECT_EQ(Scalar("SELECT COUNT(*) FROM t WHERE FALSE"), "0");
}

TEST_F(ResolvedTranslatorTest, RunsSetOperations) {
  using V = std::vector<std::string>;
  EXPECT_EQ(Column(Execute("SELECT 1 AS x UNION ALL SELECT 1 UNION ALL SELECT 2 ORDER BY x")),
            (V{"1", "1", "2"}));
  EXPECT_EQ(Column(Execute("SELECT a FROM t UNION DISTINCT SELECT a FROM t ORDER BY a")),
            (V{"NULL", "1", "2", "3"}));
  EXPECT_EQ(Column(Execute("SELECT a FROM t INTERSECT DISTINCT SELECT 2")), (V{"2"}));
  EXPECT_EQ(Column(Execute("SELECT a FROM t WHERE a IS NOT NULL EXCEPT DISTINCT "
                           "SELECT 2 ORDER BY 1")),
            (V{"1", "3"}));
}

TEST_F(ResolvedTranslatorTest, RunsAnalyticFunctionsAndQualify) {
  using V = std::vector<std::string>;
  const auto result = Execute(
      "SELECT a, ROW_NUMBER() OVER (ORDER BY a), "
      "SUM(a) OVER (ORDER BY a ROWS BETWEEN 1 PRECEDING AND CURRENT ROW), "
      "LAG(a) OVER (ORDER BY a), FIRST_VALUE(a IGNORE NULLS) OVER (ORDER BY a "
      "ROWS BETWEEN UNBOUNDED PRECEDING AND UNBOUNDED FOLLOWING) "
      "FROM t ORDER BY a");
  EXPECT_EQ(Column(result, 1), (V{"1", "2", "3", "4"}));
  EXPECT_EQ(Column(result, 2), (V{"NULL", "1", "3", "5"}));
  EXPECT_EQ(Column(result, 3), (V{"NULL", "NULL", "1", "2"}));
  EXPECT_EQ(Column(result, 4), (V{"1", "1", "1", "1"}));
  EXPECT_EQ(Column(Execute("SELECT b FROM t WHERE a IS NOT NULL "
                           "QUALIFY ROW_NUMBER() OVER (ORDER BY a DESC) = 1")),
            (V{"あ"}));
}

TEST_F(ResolvedTranslatorTest, RunsSubqueries) {
  using V = std::vector<std::string>;
  EXPECT_EQ(Scalar("SELECT (SELECT MAX(a) FROM t)"), "3");
  EXPECT_EQ(Scalar("SELECT EXISTS(SELECT 1 FROM t WHERE a = 2)"), "true");
  EXPECT_EQ(Column(Execute("SELECT a FROM t WHERE a IN (SELECT a + 1 FROM t) ORDER BY a")),
            (V{"2", "3"}));
  EXPECT_EQ(Scalar("SELECT ARRAY_TO_STRING(ARRAY(SELECT b FROM t WHERE b IS NOT NULL "
                   "ORDER BY a DESC), ',')"),
            "あ,yy,x");
  EXPECT_EQ(Scalar("SELECT ARRAY_LENGTH(ARRAY(SELECT a FROM t WHERE FALSE))"), "0");
  EXPECT_EQ(Column(Execute("SELECT (SELECT COUNT(*) FROM t AS i WHERE i.a < o.a) FROM t AS o "
                           "WHERE o.a IS NOT NULL ORDER BY o.a")),
            (V{"0", "1", "2"}));
  EXPECT_EQ(Column(Execute("SELECT a FROM t AS o WHERE EXISTS (SELECT 1 FROM (SELECT a FROM t "
                           "WHERE a > o.a)) ORDER BY a")),
            (V{"1", "2"}));
}

TEST_F(ResolvedTranslatorTest, RunsUnnestStructsAndArrays) {
  using V = std::vector<std::string>;
  const auto unnested = Execute(
      "SELECT x, o FROM UNNEST(['a', 'b']) AS x WITH OFFSET AS o "
      "ORDER BY o DESC");
  EXPECT_EQ(Column(unnested, 0), (V{"b", "a"}));
  EXPECT_EQ(Column(unnested, 1), (V{"1", "0"}));
  EXPECT_EQ(Scalar("SELECT SUM(x) FROM UNNEST([1, 2, 3]) AS x"), "6");
  EXPECT_EQ(Column(Execute("SELECT a, x FROM t LEFT JOIN UNNEST(GENERATE_ARRAY(1, a - 1)) AS x "
                           "WHERE a IS NOT NULL ORDER BY a, x")),
            (V{"1", "2", "3", "3"}));
  EXPECT_EQ(Scalar("SELECT s.y FROM (SELECT STRUCT(1 AS x, 'z' AS y) AS s)"), "z");
  EXPECT_EQ(Scalar("SELECT [10, 20, 30][OFFSET(1)]"), "20");
  EXPECT_EQ(Scalar("SELECT [10, 20, 30][ORDINAL(1)]"), "10");
  EXPECT_EQ(Scalar("SELECT [10, 20, 30][SAFE_OFFSET(3)]"), std::nullopt);
  EXPECT_THROW(Execute("SELECT [10, 20, 30][OFFSET(3)]"), BackendError);
}

TEST_F(ResolvedTranslatorTest, InsertsValuesAndQueryResults) {
  Execute("INSERT INTO t VALUES (10, 'v', b'\\x01'), (11, CONCAT('w', 'x'), NULL)");
  Execute("INSERT INTO p.ds.t (b, a, raw) VALUES ('reordered', 12, DEFAULT)");
  Execute("INSERT t (a) SELECT a + 100 FROM t WHERE a IN (1, 2) ORDER BY a");

  const auto parameters = QueryParameters::Parse(nlohmann::json::parse(R"([
    {"name":"a","parameterType":{"type":"INT64"},"parameterValue":{"value":"13"}}
  ])"));
  AnalyzerSettings settings;
  settings.named_parameters.emplace_back("a", googlesql::types::Int64Type());
  Execute("INSERT INTO t (a, b) VALUES (@a, 'param')", parameters, settings);

  const auto result =
      backend_.Execute("SELECT a, b, hex(raw) FROM p.ds.t WHERE a >= 10 ORDER BY a");
  ASSERT_EQ(result.rows.size(), 6);
  const std::vector<std::vector<std::optional<std::string>>> expected = {
      {"10", "v", "01"},
      {"11", "wx", std::nullopt},
      {"12", "reordered", std::nullopt},
      {"13", "param", std::nullopt},
      {"101", std::nullopt, std::nullopt},
      {"102", std::nullopt, std::nullopt}};
  for (size_t row = 0; row < expected.size(); ++row) {
    for (size_t i = 0; i < 3; ++i) {
      const auto& cell = result.rows[row]["f"][i]["v"];
      const auto& want = expected[row][i];
      if (want.has_value()) {
        EXPECT_EQ(cell, *want) << row << "," << i;
      } else {
        EXPECT_TRUE(cell.is_null()) << row << "," << i;
      }
    }
  }
}

TEST_F(ResolvedTranslatorTest, FallsBackForUnsupportedInsertModifiers) {
  for (const std::string& sql : {std::string("INSERT OR IGNORE INTO t (a) VALUES (1)"),
                                 std::string("INSERT INTO t (a) VALUES (1) ASSERT_ROWS_MODIFIED 1"),
                                 std::string("INSERT INTO t (a) VALUES (1) THEN RETURN a")}) {
    EXPECT_FALSE(Translate(sql).has_value()) << sql;
  }
}

TEST_F(ResolvedTranslatorTest, UpdatesAndDeletesRows) {
  using V = std::vector<std::string>;
  Execute("UPDATE t SET b = CONCAT(b, '!'), raw = DEFAULT WHERE a = 1");
  Execute("UPDATE p.ds.t AS x SET a = x.a * 10 WHERE x.a > 1");
  Execute(
      "UPDATE t SET b = s.b FROM (SELECT 30 AS a, 'from' AS b) AS s "
      "WHERE t.a = s.a");
  Execute("DELETE t WHERE a IS NULL");
  Execute("DELETE FROM t WHERE EXISTS (SELECT 1 FROM UNNEST([20]) AS v WHERE v = t.a)");
  EXPECT_EQ(
      Column(Execute("SELECT CONCAT(CAST(a AS STRING), ':', b, ':', CAST(raw IS NULL AS STRING)) "
                     "FROM t ORDER BY a")),
      (V{"1:x!:true", "30:from:false"}));
}

TEST_F(ResolvedTranslatorTest, MergesRows) {
  using V = std::vector<std::string>;
  Execute(
      "MERGE t USING (SELECT 1 AS a, 'one' AS b UNION ALL SELECT 5, 'five') AS s ON t.a = s.a "
      "WHEN MATCHED THEN UPDATE SET b = s.b "
      "WHEN NOT MATCHED BY TARGET THEN INSERT (a, b) VALUES (s.a, s.b) "
      "WHEN NOT MATCHED BY SOURCE AND t.a = 2 THEN DELETE "
      "WHEN NOT MATCHED BY SOURCE THEN UPDATE SET b = 'gone'");
  EXPECT_EQ(
      Column(Execute("SELECT CONCAT(IFNULL(CAST(a AS STRING), 'NULL'), ':', b) FROM t ORDER BY a")),
      (V{"NULL:gone", "1:one", "3:gone", "5:five"}));
  Execute(
      "MERGE INTO p.ds.t AS d USING (SELECT 5 AS a, 'new' AS b, CAST(NULL AS BYTES) AS raw "
      "UNION ALL SELECT 7, 'new', b'z') AS s ON d.a = s.a "
      "WHEN MATCHED AND d.b = 'five' THEN DELETE "
      "WHEN NOT MATCHED THEN INSERT ROW");
  EXPECT_EQ(Column(Execute("SELECT a FROM t WHERE a IS NOT NULL ORDER BY a")), (V{"1", "3", "7"}));
}

TEST_F(ResolvedTranslatorTest, DoesNotConvertParameterErrorsToFallback) {
  AnalyzerSettings settings;
  settings.named_parameters.emplace_back("s", googlesql::types::StringType());
  EXPECT_THROW(Translate("SELECT BYTE_LENGTH(@s)", {}, settings), std::exception);
}

}  // namespace
}  // namespace bigquery_emulator_duckdb
