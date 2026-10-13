#include "src/translator.h"

#include <exception>
#include <optional>
#include <string>
#include <vector>

#include "gmock/gmock.h"
#include "googlesql/public/type.h"
#include "gtest/gtest.h"
#include "nlohmann/json.hpp"
#include "src/analyzer.h"
#include "src/catalog.h"
#include "src/field_schema.h"
#include "src/query_parameters.h"
#include "src/translated_statement.h"

namespace bigquery_emulator_duckdb {
namespace {

// Translation needs only the tables' schemas.
class TestTableSource : public TableSource {
 public:
  std::optional<std::vector<FieldSchema>> FindTable(const std::string& project,
                                                    const std::string& dataset,
                                                    const std::string& table) override {
    if (project != "p" || dataset != "ds" || table != "t") {
      return std::nullopt;
    }
    return std::vector<FieldSchema>{
        {.name = "a", .type = FieldType::kInteger},
        {.name = "b", .type = FieldType::kString},
        {.name = "raw", .type = FieldType::kBytes},
    };
  }
};

class TranslatorTest : public ::testing::Test {
 protected:
  std::optional<TranslatedStatement> Statement(const std::string& sql,
                                               const QueryParameters& parameters = {},
                                               const AnalyzerSettings& settings = {}) {
    const auto analyzed = AnalyzeGoogleSql(sql, catalog_, types_, settings);
    return TranslateStatement(analyzed.statement(), parameters,
                              DefaultDataset{.project = "p", .dataset = "ds"});
  }

  std::optional<std::string> Translate(const std::string& sql,
                                       const QueryParameters& parameters = {},
                                       const AnalyzerSettings& settings = {}) {
    const auto statement = Statement(sql, parameters, settings);
    return statement ? std::optional<std::string>(statement->sql) : std::nullopt;
  }

  googlesql::TypeFactory types_;
  TestTableSource source_;
  BigQueryCatalog catalog_{source_, &types_, "p", "ds"};
};

// A STRUCT whose field names could pass for a RANGE's gets positional names in DuckDB.
TEST_F(TranslatorTest, StructFieldsCannotPassForARange) {
  const auto sql =
      Translate("SELECT STRUCT(DATE '2020-01-01' AS `$start`, DATE '2020-01-02' AS `$end`)");
  ASSERT_TRUE(sql.has_value());
  EXPECT_THAT(*sql, ::testing::HasSubstr("_field_1"));
  EXPECT_THAT(*sql, ::testing::Not(::testing::HasSubstr("\"$start\"")));
}

TEST_F(TranslatorTest, SelectsByteLengthOverloadFromResolvedType) {
  const auto sql = Translate("SELECT BYTE_LENGTH('あ') AS s, BYTE_LENGTH(b'abc') AS b");
  if (!sql.has_value()) {
    FAIL() << "Expected translation";
  }
  EXPECT_NE(sql.value().find("strlen("), std::string::npos);
  EXPECT_NE(sql.value().find("octet_length("), std::string::npos);
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
  EXPECT_EQ(view.schema.at(0).type, FieldType::kInteger);
  EXPECT_EQ(view.schema.at(1).type, FieldType::kString);
  EXPECT_EQ(view.schema.at(1).mode, FieldMode::kRepeated);
  const auto table = Statement("CREATE TABLE new_t (x INT64)");
  if (!table.has_value()) {
    FAIL() << "CREATE TABLE is not translated";
  }
  EXPECT_FALSE(table->view.has_value());
}

// An argument that a translation repeats, such as a divisor or a function argument, is still
// evaluated once.
TEST_F(TranslatorTest, EvaluatesRepeatedArgumentsOnce) {
  for (const auto* sql : {"SELECT 1 / RAND()", "SELECT LEFT('abc', CAST(RAND() * 3 AS INT64))"}) {
    const auto translated = Translate(sql);
    ASSERT_TRUE(translated.has_value()) << sql;
    const auto random = translated->find("random()");
    ASSERT_NE(random, std::string::npos) << sql;
    EXPECT_EQ(translated->find("random()", random + 1), std::string::npos) << sql;
  }
}

TEST_F(TranslatorTest, TopSumBindsVolatileWeightsOnce) {
  for (const std::string& suffix : {std::string(), std::string(" OVER ()")}) {
    const auto sql = Translate("SELECT APPROX_TOP_SUM(a, RAND(), 1)" + suffix + " FROM t");
    ASSERT_TRUE(sql.has_value());
    const auto random = sql->find("random()");
    ASSERT_NE(random, std::string::npos);
    EXPECT_EQ(sql->find("random()", random + 1), std::string::npos);
  }
}

// Invalid TVF arguments must fail before translation rather than producing a
// schema that loses an input column or cannot identify the session's range.
TEST_F(TranslatorTest, ValidatesSessionizeArguments) {
  const std::string input =
      "WITH input AS (SELECT 1 AS k, RANGE(DATE '2000-01-01', DATE '2000-01-02') AS r) ";
  for (const char* arguments : {
           "'missing', []",
           "'k', []",
           "'r', ['missing']",
           "'r', [NULL]",
           "NULL, []",
           "'r', CAST(NULL AS ARRAY<STRING>)",
           "'r', [], 'invalid'",
           "'r', [], NULL",
       }) {
    SCOPED_TRACE(arguments);
    EXPECT_THROW(
        Translate(input + "SELECT * FROM RANGE_SESSIONIZE(TABLE input, " + arguments + ")"),
        std::exception);
  }
}

TEST_F(TranslatorTest, SessionizeBindsItsInputOnce) {
  const auto sql = Translate(
      "WITH input AS (SELECT RAND() AS marker, RANGE(DATE '2000-01-01', DATE '2000-01-02') AS r) "
      "SELECT * FROM RANGE_SESSIONIZE(TABLE input, 'r', [])");
  ASSERT_TRUE(sql.has_value());
  const auto random = sql->find("random()");
  ASSERT_NE(random, std::string::npos);
  EXPECT_EQ(sql->find("random()", random + 1), std::string::npos);
}

TEST_F(TranslatorTest, DoesNotReportParameterErrorsAsUnsupported) {
  AnalyzerSettings settings;
  settings.named_parameters.emplace_back("s", googlesql::types::StringType());
  EXPECT_THROW(Translate("SELECT BYTE_LENGTH(@s)", {}, settings), std::exception);
}

}  // namespace
}  // namespace bigquery_emulator_duckdb
