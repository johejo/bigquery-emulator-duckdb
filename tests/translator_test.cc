#include "src/translator.h"

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

namespace bigquery_emulator_duckdb {
namespace {

class TestTableSource : public TableSource {
 public:
  std::optional<std::vector<FieldSchema>> FindTable(const std::string& project,
                                                    const std::string& dataset,
                                                    const std::string& table) override {
    if (project != "p" || dataset != "ds") {
      return std::nullopt;
    }
    if (table == "st") {
      return std::vector<FieldSchema>{
          {.name = "id", .type = FieldType::kInteger},
          {.name = "s",
           .type = FieldType::kRecord,
           .fields = {{.name = "x", .type = FieldType::kInteger},
                      {.name = "y",
                       .type = FieldType::kRecord,
                       .fields = {{.name = "z", .type = FieldType::kString},
                                  {.name = "w", .type = FieldType::kInteger}}}}}};
    }
    if (table != "t") {
      return std::nullopt;
    }
    return std::vector<FieldSchema>{{.name = "a", .type = FieldType::kInteger},
                                    {.name = "b", .type = FieldType::kString},
                                    {.name = "raw", .type = FieldType::kBytes}};
  }
};

class TranslatorTest : public ::testing::Test {
 protected:
  std::optional<TranslatedStatement> Statement(const std::string& sql,
                                               const QueryParameters& parameters = {},
                                               const AnalyzerSettings& settings = {}) {
    const auto analyzed = AnalyzeGoogleSql(sql, catalog_, types_, settings);
    return TranslateStatement(analyzed.statement(), parameters, DefaultDataset{"p", "ds"});
  }

  std::optional<std::string> Translate(const std::string& sql,
                                       const QueryParameters& parameters = {},
                                       const AnalyzerSettings& settings = {}) {
    const auto statement = Statement(sql, parameters, settings);
    return statement ? std::optional<std::string>(statement->sql) : std::nullopt;
  }

  std::string Unsupported(const std::string& sql) {
    const auto analyzed = AnalyzeGoogleSql(sql, catalog_, types_, {});
    std::string reason;
    if (TranslateStatement(analyzed.statement(), {}, DefaultDataset{"p", "ds"}, &reason)
            .has_value()) {
      throw std::runtime_error("Unexpected translation: " + sql);
    }
    return reason;
  }

  QueryResult Execute(const std::string& sql, const QueryParameters& parameters = {},
                      const AnalyzerSettings& settings = {}) {
    const auto analyzed = AnalyzeGoogleSql(sql, catalog_, types_, settings);
    std::string reason;
    const auto translated =
        TranslateStatement(analyzed.statement(), parameters, DefaultDataset{"p", "ds"}, &reason);
    if (!translated) {
      throw std::runtime_error("Unexpected unsupported construct (" + reason + "): " + sql);
    }
    return backend_.Execute(translated->sql);
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
    backend_.Execute(
        "CREATE TABLE p.ds.st (id BIGINT, s STRUCT(x BIGINT, y STRUCT(z VARCHAR, w BIGINT)))");
    backend_.Execute(
        "INSERT INTO p.ds.st VALUES (1, {'x': 1, 'y': {'z': 'a', 'w': 10}}), "
        "(2, {'x': 2, 'y': NULL}), (3, NULL)");
  }

  Backend backend_;
  googlesql::TypeFactory types_;
  TestTableSource source_;
  BigQueryCatalog catalog_{source_, &types_, "p", "ds"};
};

TEST_F(TranslatorTest, SelectsByteLengthOverloadFromResolvedType) {
  const auto sql = Translate("SELECT BYTE_LENGTH('あ') AS s, BYTE_LENGTH(b'abc') AS b");
  if (!sql.has_value()) {
    FAIL() << "Expected translation";
  }
  EXPECT_NE(sql.value().find("strlen("), std::string::npos);
  EXPECT_NE(sql.value().find("octet_length("), std::string::npos);
}

TEST_F(TranslatorTest, PreservesScalarTypesAndOutputOrder) {
  const auto sql = Translate("SELECT 1 AS z, 2.5 AS a, NULL AS n, TRUE AS b, b'\\x00\\xff' AS raw");
  if (!sql.has_value()) {
    FAIL() << "Expected translation";
  }
  const auto result = backend_.Execute(*sql);
  ASSERT_EQ(result.schema.size(), 5);
  ASSERT_EQ(result.rows.size(), 1);
  EXPECT_EQ(result.schema[0].name, "z");
  EXPECT_EQ(result.schema[1].name, "a");
  EXPECT_EQ(result.schema[0].type, FieldType::kInteger);
  EXPECT_EQ(result.schema[1].type, FieldType::kFloat);
  EXPECT_EQ(result.rows[0]["f"][0]["v"], "1");
  EXPECT_EQ(result.rows[0]["f"][1]["v"], "2.5");
  EXPECT_TRUE(result.rows[0]["f"][2]["v"].is_null());
  EXPECT_EQ(result.rows[0]["f"][3]["v"], "true");
  EXPECT_EQ(result.rows[0]["f"][4]["v"], "AP8=");
}

TEST_F(TranslatorTest, SubstitutesParametersAndSafeCasts) {
  const auto parameters = QueryParameters::Parse(nlohmann::json::parse(R"([
    {"name":"s","parameterType":{"type":"STRING"},"parameterValue":{"value":"あ"}}
  ])"));
  AnalyzerSettings settings;
  settings.named_parameters.emplace_back("s", googlesql::types::StringType());
  const auto sql =
      Translate("SELECT BYTE_LENGTH(@s), SAFE_CAST(@s AS INT64)", parameters, settings);
  if (!sql.has_value()) {
    FAIL() << "Expected translation";
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

TEST_F(TranslatorTest, MatchesDuplicateAliasesByColumnId) {
  const auto sql = Translate("SELECT 1 AS same, 2 AS same, 3 AS `a.b`");
  if (!sql.has_value()) {
    FAIL() << "Expected translation";
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

TEST_F(TranslatorTest, RejectsUnsupportedConstructs) {
  for (const std::string& sql :
       {std::string("SELECT SAFE.RAND()"), std::string("SELECT BYTE_LENGTH('abc'), SESSION_USER()"),
        std::string("SELECT STRUCT(1, 2)")}) {
    EXPECT_FALSE(Translate(sql).has_value()) << sql;
  }
}

TEST_F(TranslatorTest, NamesTheUnsupportedConstruct) {
  EXPECT_EQ(Unsupported("SELECT BYTE_LENGTH('abc'), SESSION_USER()"), "function SESSION_USER");
  EXPECT_EQ(Unsupported("SELECT a FROM t WHERE a IN (SELECT SAFE.RAND() FROM t)"), "SAFE.RAND");
  EXPECT_EQ(Unsupported("UPDATE t SET a = 1 WHERE TRUE ASSERT_ROWS_MODIFIED 1"),
            "UPDATE with ASSERT_ROWS_MODIFIED, THEN RETURN or generated columns");
  EXPECT_EQ(Unsupported("CREATE TEMP TABLE tmp (x INT64)"), "temporary tables");
  EXPECT_EQ(Unsupported("CREATE TEMP VIEW tmp AS SELECT a FROM t"),
            "temporary, recursive or value-table views");
  EXPECT_EQ(Unsupported("CREATE TABLE ds.g (x INT64, y INT64 AS (x + 1))"), "generated columns");
  EXPECT_EQ(Unsupported("CREATE TABLE ds.n (x BIGNUMERIC(76, 38))"), "column type BIGNUMERIC");
  EXPECT_EQ(Unsupported("CREATE TABLE ds.c (x INT64 NOT NULL) AS SELECT 1 AS x"),
            "NOT NULL in CREATE TABLE AS SELECT");
  EXPECT_EQ(Unsupported("CREATE OR REPLACE SCHEMA other"), "CREATE OR REPLACE SCHEMA");
}

TEST_F(TranslatorTest, TranslatesDdlPaths) {
  EXPECT_EQ(Translate("CREATE TABLE new_t (x INT64)"),
            "CREATE TABLE \"p\".\"ds\".\"new_t\" (\"x\" BIGINT)");
  EXPECT_EQ(Translate("CREATE TABLE IF NOT EXISTS `q.other.new_t` (x INT64)"),
            "CREATE TABLE IF NOT EXISTS \"q\".\"other\".\"new_t\" (\"x\" BIGINT)");
  EXPECT_EQ(Translate("CREATE SCHEMA IF NOT EXISTS `q.other`"),
            "CREATE SCHEMA IF NOT EXISTS \"q\".\"other\"");
  EXPECT_EQ(Translate("DROP SCHEMA other CASCADE"), "DROP SCHEMA \"p\".\"other\" CASCADE");
  EXPECT_EQ(Translate("DROP TABLE IF EXISTS ds.new_t"),
            "DROP TABLE IF EXISTS \"p\".\"ds\".\"new_t\"");
}

// The emulator records a view's GoogleSQL query and BigQuery schema, which DuckDB does not keep.
TEST_F(TranslatorTest, DescribesTheViewACreateViewDefines) {
  const auto statement = Statement("CREATE VIEW IF NOT EXISTS v AS SELECT a, [b] AS bs FROM t");
  if (!statement.has_value() || !statement->view.has_value()) {
    FAIL() << "CREATE VIEW defines no view";
  }
  const ViewDefinition& view = *statement->view;
  EXPECT_EQ(view.table.project_id, "p");
  EXPECT_EQ(view.table.dataset_id, "ds");
  EXPECT_EQ(view.table.table_id, "v");
  EXPECT_EQ(view.query, "SELECT a, [b] AS bs FROM t");
  EXPECT_TRUE(view.if_not_exists);
  ASSERT_EQ(view.schema.size(), 2);
  EXPECT_EQ(view.schema[0].type, FieldType::kInteger);
  EXPECT_EQ(view.schema[1].type, FieldType::kString);
  EXPECT_EQ(view.schema[1].mode, FieldMode::kRepeated);
  const auto table = Statement("CREATE TABLE new_t (x INT64)");
  if (!table.has_value()) {
    FAIL() << "CREATE TABLE is not translated";
  }
  EXPECT_FALSE(table->view.has_value());
}

TEST_F(TranslatorTest, RunsDdl) {
  Execute("CREATE SCHEMA other");
  Execute(
      "CREATE TABLE other.typed (id INT64 NOT NULL, price NUMERIC(10, 2), label STRING(8) "
      "DEFAULT 'none', tags ARRAY<STRING>, s STRUCT<x INT64, y ARRAY<BIGNUMERIC>>, "
      "PRIMARY KEY (id) NOT ENFORCED) PARTITION BY RANGE_BUCKET(id, GENERATE_ARRAY(0, 100, 10)) "
      "OPTIONS (description = 'd')");
  backend_.Execute("INSERT INTO p.other.typed (id, price) VALUES (1, 1.235)");
  EXPECT_THROW(backend_.Execute("INSERT INTO p.other.typed (price) VALUES (1)"), BackendError);
  auto result = backend_.Execute("SELECT * FROM p.other.typed");
  ASSERT_EQ(result.rows.size(), 1);
  EXPECT_EQ(result.rows[0]["f"][1]["v"], "1.24");
  EXPECT_EQ(result.rows[0]["f"][2]["v"], "none");
  EXPECT_EQ(result.schema[3].mode, FieldMode::kRepeated);
  EXPECT_EQ(result.schema[4].type, FieldType::kRecord);

  EXPECT_THROW(Execute("CREATE TABLE other.typed (x INT64)"), BackendError);
  Execute("CREATE TABLE IF NOT EXISTS other.typed (x INT64)");
  Execute("CREATE OR REPLACE TABLE other.typed (x INT64)");
  EXPECT_EQ(backend_.Execute("SELECT * FROM p.other.typed").schema.size(), 1);

  Execute(
      "CREATE TABLE ds.copied (n INT64, label STRING) CLUSTER BY n AS "
      "SELECT a * 10, b FROM t WHERE a IS NOT NULL ORDER BY a");
  result = backend_.Execute("SELECT * FROM p.ds.copied");
  ASSERT_EQ(result.rows.size(), 3);
  EXPECT_EQ(result.schema[0].name, "n");
  EXPECT_EQ(result.schema[1].name, "label");
  EXPECT_EQ(result.rows[0]["f"][0]["v"], "10");

  Execute("CREATE OR REPLACE TABLE ds.copied AS SELECT COUNT(*) AS c FROM t");
  EXPECT_EQ(backend_.Execute("SELECT c FROM p.ds.copied").rows[0]["f"][0]["v"], "4");

  Execute("DROP TABLE ds.copied");
  Execute("DROP TABLE IF EXISTS ds.copied");
  EXPECT_THROW(Execute("DROP TABLE ds.copied"), BackendError);
  EXPECT_THROW(Execute("DROP SCHEMA other"), BackendError);
  Execute("DROP SCHEMA other CASCADE");
  Execute("DROP SCHEMA IF EXISTS other");
}

TEST_F(TranslatorTest, RunsSafeCalls) {
  // Only the function's own errors become NULL, not those of its arguments.
  EXPECT_THROW(Execute("SELECT SAFE.ABS(1 / 0)"), BackendError);
}

TEST_F(TranslatorTest, ReadsTablesAndKeepsHiddenSortColumns) {
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
  EXPECT_EQ(result.schema[0].type, FieldType::kInteger);
}

TEST_F(TranslatorTest, ResolvesScopesAliasesAndStarModifiers) {
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

TEST_F(TranslatorTest, PreservesCardinalityWithoutReferencedTableColumns) {
  EXPECT_EQ(Execute("SELECT 42 FROM t").rows.size(), 4);
  EXPECT_TRUE(Execute("SELECT 42 FROM t WHERE FALSE").rows.empty());
  EXPECT_TRUE(Execute("SELECT a FROM t LIMIT 0").rows.empty());
  EXPECT_EQ(Execute("SELECT 42 FROM t LIMIT 2").rows.size(), 2);
}

TEST_F(TranslatorTest, SortsNullsAndAppliesLimitsAtTheCorrectScope) {
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

TEST_F(TranslatorTest, RunsOperatorsAndConditions) {
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
  const auto division = Translate("SELECT 1 / RAND()");
  if (!division) {
    FAIL() << "Expected translation";
  }
  const auto random = division->find("random()");
  ASSERT_NE(random, std::string::npos);
  EXPECT_EQ(division->find("random()", random + 1), std::string::npos);
  // An argument that a function translation repeats is still evaluated once.
  const auto left = Translate("SELECT LEFT('abc', CAST(RAND() * 3 AS INT64))");
  if (!left) {
    FAIL() << "Expected translation";
  }
  const auto left_random = left->find("random()");
  ASSERT_NE(left_random, std::string::npos);
  EXPECT_EQ(left->find("random()", left_random + 1), std::string::npos);
  EXPECT_EQ(Execute("SELECT IF(a = 1, 42, 1 / (a - 1)) FROM t WHERE a = 1").rows.at(0)["f"][0]["v"],
            "42.0");
  EXPECT_EQ(Execute("SELECT IF(FALSE, 1 / 0, 42)").rows.at(0)["f"][0]["v"], "42.0");
}

TEST_F(TranslatorTest, PreparesParameterizedTableQueries) {
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
    FAIL() << "Expected translation";
  }
  const auto prepared = backend_.Prepare(*sql);
  ASSERT_EQ(prepared.schema.size(), 1);
  EXPECT_EQ(prepared.schema[0].name, "x");
  EXPECT_EQ(prepared.schema[0].type, FieldType::kInteger);
  const auto result = backend_.Execute(*sql);
  ASSERT_EQ(result.rows.size(), 1);
  EXPECT_EQ(result.rows[0]["f"][0]["v"], "3");
}

// Each case is an edge where DuckDB's counterpart answers differently from BigQuery.
TEST_F(TranslatorTest, RunsStringBytesAndMathFunctions) {
  EXPECT_THROW(Execute("SELECT LEFT('abc', -1)"), BackendError);
}

TEST_F(TranslatorTest, RunsSubstrAtBigQueryPositions) {
  EXPECT_THROW(Execute("SELECT SUBSTR('abc', 1, -1)"), BackendError);
}

// The BYTES overloads work on hexadecimal digits, so a byte that is not valid UTF-8 or looks
// like part of another byte's digits must stay whole.
TEST_F(TranslatorTest, RunsStringFunctionsOnBytes) {
  const auto hex = [this](const std::string& expression) {
    return Scalar("SELECT TO_HEX(" + expression + ")");
  };
  EXPECT_EQ(hex("LEFT(b'\\xffab', 2)"), "ff61");
  EXPECT_EQ(hex("RIGHT(b'\\xffab', 1)"), "62");
  EXPECT_THROW(Execute("SELECT LEFT(b'abc', -1)"), BackendError);
  EXPECT_EQ(hex("SUBSTR(b'\\xffab', 2)"), "6162");
  EXPECT_EQ(hex("SUBSTR(b'\\xffab', 0, 2)"), "ff61");
  EXPECT_EQ(hex("SUBSTRING(b'\\xffab', -2, 1)"), "61");
  EXPECT_EQ(hex("REVERSE(b'\\x01\\xffa')"), "61ff01");
  EXPECT_EQ(hex("LOWER(b'\\\\\\xffAz')"), "5cff617a");
  EXPECT_EQ(hex("UPPER(b'\\\\\\xffAz')"), "5cff415a");
  EXPECT_EQ(hex("LPAD(b'a', 4, b'\\xff')"), "ffffff61");
  EXPECT_EQ(hex("RPAD(b'a', 2)"), "6120");
  EXPECT_EQ(hex("LPAD(b'abc', 2)"), "6162");
  EXPECT_EQ(hex("TRIM(b'\\xffa\\xffb\\xff', b'\\xff')"), "61ff62");
  EXPECT_EQ(hex("LTRIM(b'aab', b'a')"), "62");
  EXPECT_EQ(hex("RTRIM(b'abb', b'ab')"), "");
  // 0x1f 0xf1 holds the digits of 0xff between its bytes.
  EXPECT_EQ(hex("REPLACE(b'\\x1f\\xf1\\xff', b'\\xff', b'z')"), "1ff17a");
  EXPECT_EQ(hex("REPLACE(b'abc', b'', b'z')"), "616263");
  EXPECT_EQ(hex("ARRAY_TO_STRING([b'a', NULL, b'\\xff'], b'-')"), "612dff");
  EXPECT_EQ(hex("ARRAY_TO_STRING([b'a', NULL], b'-', b'\\xff')"), "612dff");
}

// src/backend_functions.cc registers GoogleSQL's implementations of these.
TEST_F(TranslatorTest, RunsGoogleSqlFunctions) {
  EXPECT_THROW(Execute("SELECT REGEXP_INSTR('abc', 'b', 0)"), BackendError);
  EXPECT_THROW(Execute("SELECT REGEXP_INSTR('abc', 'b', 1, 1, 2)"), BackendError);
  EXPECT_THROW(Execute("SELECT REGEXP_INSTR('abc', '(')"), BackendError);
}

// Weeks, sub-second parts and differences below a day are where DuckDB's date functions answer
// differently from BigQuery's.
TEST_F(TranslatorTest, RunsDateTimeConstructorsAndExtract) {
  EXPECT_EQ(Unsupported("SELECT GENERATE_DATE_ARRAY(DATE '2024-01-31', DATE '2024-05-01', "
                        "INTERVAL 1 MONTH)"),
            "function GENERATE_DATE_ARRAY");
  EXPECT_THROW(Execute("SELECT GENERATE_TIMESTAMP_ARRAY(TIMESTAMP '2024-01-01', "
                       "TIMESTAMP '2024-01-02', INTERVAL 0 DAY)"),
               BackendError);
}

TEST_F(TranslatorTest, Buckets) {
  EXPECT_THROW(Execute("SELECT DATE_BUCKET(DATE '2024-01-01', INTERVAL -1 DAY)"), BackendError);
  EXPECT_THROW(Execute("SELECT TIMESTAMP_BUCKET(TIMESTAMP '2024-01-01', INTERVAL 0 HOUR)"),
               BackendError);
  EXPECT_EQ(Unsupported("SELECT TIMESTAMP_BUCKET(TIMESTAMP '2024-01-01', INTERVAL 1 MONTH)"),
            "function TIMESTAMP_BUCKET");
  EXPECT_EQ(Unsupported("SELECT DATE_BUCKET(DATE '2024-01-01', INTERVAL '1 2' DAY TO HOUR)"),
            "function DATE_BUCKET bucket width");
}

std::vector<std::string> Column(const QueryResult& result, size_t index = 0) {
  std::vector<std::string> values;
  for (const auto& row : result.rows) {
    const auto& cell = row["f"][index]["v"];
    values.push_back(cell.is_null() ? "NULL" : cell.get<std::string>());
  }
  return values;
}

// Each row's cells joined with '|', so a test can compare whole rows.
std::vector<std::string> Rows(const QueryResult& result) {
  std::vector<std::string> rows(result.rows.size());
  for (size_t i = 0; i < result.schema.size(); ++i) {
    const auto column = Column(result, i);
    for (size_t j = 0; j < rows.size(); ++j) {
      rows[j] += (i == 0 ? "" : "|") + column[j];
    }
  }
  return rows;
}

TEST_F(TranslatorTest, RunsJoinsAndCtes) {
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

TEST_F(TranslatorTest, RunsAggregatesAndDistinct) {
  using V = std::vector<std::string>;
  const auto result = Execute(
      "SELECT SUM(a), COUNT(*), COUNT(a), COUNT(DISTINCT a), AVG(a), MIN(b), MAX(b), "
      "STRING_AGG(b, ',' ORDER BY a DESC), ARRAY_LENGTH(ARRAY_AGG(a IGNORE NULLS)), "
      "COUNTIF(a > 1), LOGICAL_AND(a > 0) FROM t");
  ASSERT_EQ(result.rows.size(), 1);
  EXPECT_EQ(result.schema[0].type, FieldType::kInteger);
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
}

TEST_F(TranslatorTest, RunsAggregateLimitAndHavingModifiers) {
  using V = std::vector<std::string>;
  EXPECT_EQ(Column(Execute("SELECT MOD(a, 2) AS k, ANY_VALUE(b HAVING MIN a), COUNT(*) FROM t "
                           "WHERE a IS NOT NULL GROUP BY k ORDER BY k"),
                   1),
            (V{"yy", "x"}));
}

TEST_F(TranslatorTest, RunsValueTables) {
  using V = std::vector<std::string>;
  const auto structs =
      Execute("SELECT AS STRUCT a, b AS name FROM t WHERE a IS NOT NULL ORDER BY a DESC");
  ASSERT_EQ(structs.schema.size(), 2);
  EXPECT_EQ(structs.schema[0].name, "a");
  EXPECT_EQ(structs.schema[1].name, "name");
  EXPECT_EQ(Rows(structs), (V{"3|あ", "2|yy", "1|x"}));
  EXPECT_EQ(Rows(Execute("SELECT AS VALUE a FROM t WHERE a > 1 ORDER BY a")), (V{"2", "3"}));
  EXPECT_EQ(Rows(Execute("SELECT AS VALUE STRUCT(a AS x) FROM t WHERE a = 1")), (V{"1"}));
}

TEST_F(TranslatorTest, RunsGroupingSets) {
  using V = std::vector<std::string>;
  EXPECT_EQ(Rows(Execute("SELECT a > 1 AS big, COUNT(*) AS n, GROUPING(a > 1) AS g FROM t "
                         "WHERE a IS NOT NULL GROUP BY ROLLUP(big) ORDER BY g, big")),
            (V{"false|1|0", "true|2|0", "NULL|3|1"}));
  EXPECT_EQ(Rows(Execute("SELECT a, b, COUNT(*) FROM t WHERE a < 3 "
                         "GROUP BY CUBE(a, b) ORDER BY a NULLS LAST, b NULLS LAST")),
            (V{"1|x|1", "1|NULL|1", "2|yy|1", "2|NULL|1", "NULL|x|1", "NULL|yy|1", "NULL|NULL|2"}));
  EXPECT_EQ(Rows(Execute("SELECT a, b, SUM(a) FROM t WHERE a < 3 "
                         "GROUP BY GROUPING SETS ((a, b), a, ()) ORDER BY 3, 1, 2")),
            (V{"1|NULL|1", "1|x|1", "2|NULL|2", "2|yy|2", "NULL|NULL|3"}));
  EXPECT_EQ(Rows(Execute("SELECT a, b, COUNT(*) FROM t WHERE a < 3 "
                         "GROUP BY a, ROLLUP(b) ORDER BY a, b NULLS LAST")),
            (V{"1|x|1", "1|NULL|1", "2|yy|1", "2|NULL|1"}));
  EXPECT_EQ(Rows(Execute("SELECT a, GROUPING(a) FROM t WHERE a = 1 GROUP BY a")), (V{"1|0"}));
}

TEST_F(TranslatorTest, RunsRecursiveCtes) {
  using V = std::vector<std::string>;
  EXPECT_EQ(Column(Execute("WITH RECURSIVE c AS (SELECT 1 AS n UNION ALL "
                           "SELECT n + 1 FROM c WHERE n < 3) SELECT n FROM c ORDER BY n")),
            (V{"1", "2", "3"}));
  // UNION DISTINCT stops once an iteration adds no new row.
  EXPECT_EQ(Column(Execute("WITH RECURSIVE c AS (SELECT 0 AS n UNION DISTINCT "
                           "SELECT MOD(n + 1, 3) FROM c) SELECT n FROM c ORDER BY n")),
            (V{"0", "1", "2"}));
  // A plain entry next to a recursive one, and a recursive term that joins a table and
  // filters through a subquery.
  EXPECT_EQ(Rows(Execute("WITH RECURSIVE base AS (SELECT a FROM t WHERE a IS NOT NULL), "
                         "c AS (SELECT MIN(a) AS n, 1 AS depth FROM base UNION ALL "
                         "SELECT base.a, depth + 1 FROM c JOIN base ON base.a = c.n + 1 "
                         "WHERE EXISTS (SELECT 1 FROM base WHERE a > c.n)) "
                         "SELECT n, depth FROM c ORDER BY depth")),
            (V{"1|1", "2|2", "3|3"}));
  EXPECT_EQ(Unsupported("WITH RECURSIVE c AS (SELECT 1 AS n UNION ALL "
                        "SELECT n + 1 FROM c WHERE n < 3) WITH DEPTH SELECT n FROM c"),
            "WITH RECURSIVE depth modifier");
}

TEST_F(TranslatorTest, RunsSetOperations) {
  using V = std::vector<std::string>;
  EXPECT_EQ(Column(Execute("SELECT 1 AS x UNION ALL SELECT 1 UNION ALL SELECT 2 ORDER BY x")),
            (V{"1", "1", "2"}));
  EXPECT_EQ(Column(Execute("SELECT a FROM t UNION DISTINCT SELECT a FROM t ORDER BY a")),
            (V{"NULL", "1", "2", "3"}));
  EXPECT_EQ(Column(Execute("SELECT a FROM t INTERSECT DISTINCT SELECT 2")), (V{"2"}));
  EXPECT_EQ(Column(Execute("SELECT a FROM t WHERE a IS NOT NULL EXCEPT DISTINCT "
                           "SELECT 2 ORDER BY 1")),
            (V{"1", "3"}));
  EXPECT_EQ(Rows(Execute("SELECT 1 AS a, 2 AS b UNION ALL CORRESPONDING SELECT 3 AS b, 4 AS a "
                         "ORDER BY a")),
            (V{"1|2", "4|3"}));
  EXPECT_EQ(Rows(Execute("SELECT 1 AS a, 2 AS b FULL UNION ALL CORRESPONDING SELECT 3 AS c, "
                         "4 AS a ORDER BY a")),
            (V{"1|2|NULL", "4|NULL|3"}));
  EXPECT_EQ(Rows(Execute("SELECT 1 AS a, 2 AS b LEFT UNION ALL CORRESPONDING SELECT 3 AS c, "
                         "4 AS a ORDER BY a")),
            (V{"1|2", "4|NULL"}));
  EXPECT_EQ(Rows(Execute("SELECT 1 AS a, 2 AS b INNER UNION ALL CORRESPONDING SELECT 3 AS c, "
                         "4 AS a ORDER BY a")),
            (V{"1", "4"}));
  EXPECT_EQ(Rows(Execute("SELECT 1 AS a, 2 AS b, 5 AS c UNION ALL CORRESPONDING BY (c, a) "
                         "SELECT 3 AS c, 4 AS a ORDER BY a")),
            (V{"5|1", "3|4"}));
  EXPECT_EQ(Column(Execute("SELECT a, b FROM t INTERSECT DISTINCT CORRESPONDING "
                           "SELECT 'x' AS b, 1 AS a")),
            (V{"1"}));
}

TEST_F(TranslatorTest, RunsLateralJoins) {
  using V = std::vector<std::string>;
  EXPECT_EQ(Rows(Execute("SELECT t.a, s.m FROM t, LATERAL (SELECT MAX(u.a) AS m FROM t AS u "
                         "WHERE u.a < t.a) AS s ORDER BY t.a")),
            (V{"NULL|NULL", "1|NULL", "2|1", "3|2"}));
  EXPECT_EQ(Rows(Execute("SELECT t.a, s.b FROM t LEFT JOIN LATERAL (SELECT u.b FROM t AS u "
                         "WHERE u.a = t.a + 1) AS s ON s.b != 'yy' ORDER BY t.a")),
            (V{"NULL|NULL", "1|NULL", "2|あ", "3|NULL"}));
  EXPECT_EQ(Column(Execute("SELECT t.a, s.n FROM t JOIN LATERAL (SELECT COUNT(*) AS n FROM t AS u "
                           "WHERE u.a <= t.a) AS s ON s.n > 1 ORDER BY t.a"),
                   1),
            (V{"2", "3"}));
  // The lateral side also sees columns of the query enclosing the join.
  EXPECT_EQ(Column(Execute("SELECT (SELECT SUM(s.x) FROM UNNEST([10, 20]) AS o, "
                           "LATERAL (SELECT o + t.a AS x) AS s) FROM t ORDER BY t.a")),
            (V{"NULL", "32", "34", "36"}));
}

TEST_F(TranslatorTest, RunsAnalyticFunctionsAndQualify) {
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
  EXPECT_EQ(Rows(Execute("SELECT a, COUNT(DISTINCT MOD(a, 2)) OVER (PARTITION BY a > 1), "
                         "SUM(DISTINCT a) OVER () FROM (SELECT a FROM t UNION ALL SELECT a FROM t) "
                         "WHERE a IS NOT NULL ORDER BY a")),
            (V{"1|1|6", "1|1|6", "2|2|6", "2|2|6", "3|2|6", "3|2|6"}));
}

TEST_F(TranslatorTest, RunsSubqueries) {
  using V = std::vector<std::string>;
  EXPECT_EQ(Column(Execute("SELECT a FROM t WHERE a IN (SELECT a + 1 FROM t) ORDER BY a")),
            (V{"2", "3"}));
  EXPECT_EQ(Column(Execute("SELECT (SELECT COUNT(*) FROM t AS i WHERE i.a < o.a) FROM t AS o "
                           "WHERE o.a IS NOT NULL ORDER BY o.a")),
            (V{"0", "1", "2"}));
  EXPECT_EQ(Column(Execute("SELECT a FROM t AS o WHERE EXISTS (SELECT 1 FROM (SELECT a FROM t "
                           "WHERE a > o.a)) ORDER BY a")),
            (V{"1", "2"}));
}

TEST_F(TranslatorTest, RunsUnnestStructsAndArrays) {
  using V = std::vector<std::string>;
  const auto unnested = Execute(
      "SELECT x, o FROM UNNEST(['a', 'b']) AS x WITH OFFSET AS o "
      "ORDER BY o DESC");
  EXPECT_EQ(Column(unnested, 0), (V{"b", "a"}));
  EXPECT_EQ(Column(unnested, 1), (V{"1", "0"}));
  EXPECT_EQ(Column(Execute("SELECT a, x FROM t LEFT JOIN UNNEST(GENERATE_ARRAY(1, a - 1)) AS x "
                           "WHERE a IS NOT NULL ORDER BY a, x")),
            (V{"1", "2", "3", "3"}));
  EXPECT_EQ(Rows(Execute("SELECT x, y, o FROM UNNEST([1, 2, 3] AS x, ['a'] AS y) WITH OFFSET AS o "
                         "ORDER BY o")),
            (V{"1|a|0", "2|NULL|1", "3|NULL|2"}));
  EXPECT_EQ(Rows(Execute("SELECT x, y FROM UNNEST([1, 2, 3] AS x, ['a', 'b'] AS y, "
                         "mode => 'TRUNCATE') ORDER BY x")),
            (V{"1|a", "2|b"}));
  EXPECT_EQ(Rows(Execute("SELECT a, x, y FROM t, UNNEST([a] AS x, CAST(NULL AS ARRAY<STRING>) "
                         "AS y) WHERE a IS NOT NULL ORDER BY a")),
            (V{"1|1|NULL", "2|2|NULL", "3|3|NULL"}));
  EXPECT_THROW(Execute("SELECT x FROM UNNEST([1, 2] AS x, [3] AS y, mode => 'STRICT')"),
               BackendError);
  EXPECT_THROW(Execute("SELECT [10, 20, 30][OFFSET(3)]"), BackendError);
}

TEST_F(TranslatorTest, InsertsValuesAndQueryResults) {
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

TEST_F(TranslatorTest, RejectsUnsupportedInsertModifiers) {
  for (const std::string& sql : {std::string("INSERT OR IGNORE INTO t (a) VALUES (1)"),
                                 std::string("INSERT INTO t (a) VALUES (1) ASSERT_ROWS_MODIFIED 1"),
                                 std::string("INSERT INTO t (a) VALUES (1) THEN RETURN a")}) {
    EXPECT_FALSE(Translate(sql).has_value()) << sql;
  }
}

TEST_F(TranslatorTest, UpdatesAndDeletesRows) {
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

// A struct field assignment rebuilds the struct around it, keeping the fields it leaves alone.
TEST_F(TranslatorTest, UpdatesStructFields) {
  using V = std::vector<std::string>;
  Execute("UPDATE st SET s.x = s.x + 100, s.y.z = 'b' WHERE id = 1");
  Execute("UPDATE st SET s.y = STRUCT('c', 20) WHERE id = 2");
  EXPECT_EQ(Column(Execute("SELECT FORMAT('%d:%s:%d', s.x, s.y.z, s.y.w) FROM st "
                           "WHERE id < 3 ORDER BY id")),
            (V{"101:b:10", "2:c:20"}));
  EXPECT_THROW(Execute("UPDATE st SET s.x = 1 WHERE id = 3"), BackendError);
}

TEST_F(TranslatorTest, MergesRows) {
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

TEST_F(TranslatorTest, RunsOperatorsAndArrayFunctions) {
  EXPECT_THROW(Execute("SELECT 1 << -1"), std::exception);
  EXPECT_THROW(Execute("SELECT ARRAY_FIRST(CAST([] AS ARRAY<INT64>))"), std::exception);
  EXPECT_THROW(Execute("SELECT ERROR('boom')"), std::exception);
}

TEST_F(TranslatorTest, RunsRegularExpressionFunctions) {
  EXPECT_THROW(Execute("SELECT REGEXP_EXTRACT('ab', r'(a)(b)')"), std::exception);
  EXPECT_EQ(Unsupported("SELECT REGEXP_EXTRACT(b, b) FROM p.ds.t"), "function REGEXP_EXTRACT");
  EXPECT_EQ(Unsupported("SELECT REGEXP_EXTRACT('a', 'a', 2)"), "function REGEXP_EXTRACT");
  EXPECT_EQ(Unsupported("SELECT REGEXP_EXTRACT(b'a', b'a')"), "function REGEXP_EXTRACT");
}

TEST_F(TranslatorTest, RunsJsonFunctions) {
  const std::string doc = R"('{"a": {"b": [1, null, {"c": "x"}, "s"]}, "k.l": 2, "n": null}')";
  EXPECT_EQ(Scalar("SELECT JSON_QUERY(" + doc + ", '$.a.b[2]')"), R"({"c":"x"})");
  EXPECT_EQ(Scalar("SELECT JSON_QUERY(" + doc + ", '$.\"k.l\"')"), "2");
  EXPECT_EQ(Scalar("SELECT JSON_QUERY(" + doc + ", '$.n')"), std::nullopt);
  EXPECT_EQ(Scalar("SELECT JSON_QUERY(" + doc + ", '$.missing')"), std::nullopt);
  EXPECT_EQ(Scalar("SELECT JSON_EXTRACT(" + doc + ", \"$['k.l']\")"), "2");
  EXPECT_EQ(Scalar("SELECT JSON_VALUE(" + doc + ", '$.a.b[3]')"), "s");
  EXPECT_EQ(Scalar("SELECT JSON_VALUE(" + doc + ", '$.a.b[0]')"), "1");
  EXPECT_EQ(Scalar("SELECT JSON_VALUE(" + doc + ", '$.a')"), std::nullopt);
  EXPECT_EQ(Scalar("SELECT TO_JSON_STRING(JSON_QUERY_ARRAY(" + doc + ", '$.a.b'))"),
            R"(["1","null","{\"c\":\"x\"}","\"s\""])");
  EXPECT_EQ(Scalar("SELECT JSON_EXTRACT_ARRAY(" + doc + ", '$.a') IS NULL"), "true");
  EXPECT_EQ(Scalar("SELECT JSON_VALUE_ARRAY(" + doc + ", '$.a.b') IS NULL"), "true");
  EXPECT_EQ(Unsupported("SELECT JSON_QUERY('{}', '$[a]')"), "function JSON_QUERY");
  EXPECT_EQ(Unsupported("SELECT JSON_QUERY('{}', '$.a[*]')"), "function JSON_QUERY");
}

TEST_F(TranslatorTest, BuildsAndChangesJson) {
  EXPECT_THROW(Execute("SELECT JSON_OBJECT(['a'], [1, 2])"), BackendError);
  EXPECT_THROW(Execute("SELECT JSON_OBJECT(CAST(NULL AS STRING), 1)"), BackendError);

  const std::string doc = R"(JSON '{"a": {"b": 1, "c": null}, "d": [1, null, 2]}')";
  EXPECT_EQ(Scalar("SELECT TO_JSON_STRING(JSON_REMOVE(" + doc + ", '$.a.b', '$.d[0]'))"),
            R"({"a":{"c":null},"d":[null,2]})");
  EXPECT_EQ(Scalar("SELECT TO_JSON_STRING(JSON_REMOVE(" + doc + ", '$.x', NULL))"),
            R"({"a":{"b":1,"c":null},"d":[1,null,2]})");
  EXPECT_THROW(Execute("SELECT JSON_REMOVE(CAST(NULL AS JSON), 'a')"), BackendError);

  EXPECT_EQ(Scalar("SELECT TO_JSON_STRING(JSON_SET(" + doc + ", '$.a.b', 'x', '$.e.f', [1]))"),
            R"({"a":{"b":"x","c":null},"d":[1,null,2],"e":{"f":[1]}})");
  EXPECT_EQ(
      Scalar("SELECT TO_JSON_STRING(JSON_SET(" + doc + ", '$.e', 1, create_if_missing => FALSE))"),
      R"({"a":{"b":1,"c":null},"d":[1,null,2]})");
  EXPECT_EQ(Scalar("SELECT TO_JSON_STRING(JSON_SET(" + doc + ", '$.a.b.c', 1))"),
            R"({"a":{"b":1,"c":null},"d":[1,null,2]})");
  EXPECT_THROW(Execute("SELECT JSON_SET(" + doc + ", 'a', 1)"), BackendError);

  EXPECT_EQ(Scalar("SELECT TO_JSON_STRING(JSON_STRIP_NULLS(" + doc + "))"),
            R"({"a":{"b":1},"d":[1,2]})");
  EXPECT_EQ(Scalar("SELECT TO_JSON_STRING(JSON_STRIP_NULLS(" + doc +
                   ", '$.a', include_arrays => FALSE))"),
            R"({"a":{"b":1},"d":[1,null,2]})");

  EXPECT_EQ(Scalar("SELECT ARRAY_TO_STRING(JSON_KEYS(" + doc + "), ',')"), "a,a.b,a.c,d");
  EXPECT_EQ(Scalar("SELECT ARRAY_TO_STRING(JSON_KEYS(" + doc + ", 1), ',')"), "a,d");
  EXPECT_THROW(Execute("SELECT JSON_KEYS(" + doc + ", 0)"), BackendError);
}

TEST_F(TranslatorTest, AccessesAndConvertsJson) {
  const std::string doc = R"(JSON '{"a": {"b": [10, 20]}, "c d": "x", "f": 2.0, "g": 2.5}')";
  EXPECT_EQ(Scalar("SELECT TO_JSON_STRING((" + doc + ").a.b)"), "[10,20]");
  EXPECT_EQ(Scalar("SELECT TO_JSON_STRING((" + doc + ").a.b[1])"), "20");
  EXPECT_EQ(Scalar("SELECT (" + doc + ").a.b[5] IS NULL"), "true");
  EXPECT_EQ(Scalar("SELECT (" + doc + ").a.b[-1] IS NULL"), "true");
  EXPECT_EQ(Scalar("SELECT STRING((" + doc + ")['c d'])"), "x");
  EXPECT_EQ(Scalar("SELECT INT64((" + doc + ").a.b[0])"), "10");
  EXPECT_EQ(Scalar("SELECT INT64((" + doc + ").f)"), "2");
  EXPECT_THROW(Execute("SELECT INT64((" + doc + ").g)"), std::exception);
  EXPECT_THROW(Execute("SELECT INT64((" + doc + ").a)"), std::exception);
  EXPECT_EQ(Scalar("SELECT FLOAT64((" + doc + ").g)"), "2.5");
  EXPECT_THROW(Execute("SELECT BOOL(JSON '1')"), std::exception);
  EXPECT_THROW(Execute("SELECT STRING(JSON '1')"), std::exception);
  EXPECT_EQ(Scalar("SELECT STRING((" + doc + ").missing)"), std::nullopt);
  EXPECT_EQ(Unsupported("SELECT FLOAT64(JSON '1', wide_number_mode => 'exact')"),
            "function FLOAT64");
}

TEST_F(TranslatorTest, DoesNotReportParameterErrorsAsUnsupported) {
  AnalyzerSettings settings;
  settings.named_parameters.emplace_back("s", googlesql::types::StringType());
  EXPECT_THROW(Translate("SELECT BYTE_LENGTH(@s)", {}, settings), std::exception);
}

}  // namespace
}  // namespace bigquery_emulator_duckdb
