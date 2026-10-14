#include "src/translator.h"

#include <optional>
#include <string>
#include <vector>

#include "googlesql/public/type.h"
#include "gtest/gtest.h"
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
  std::optional<std::string> Translate(const std::string& sql) {
    const auto analyzed = AnalyzeGoogleSql(sql, catalog_, types_, AnalyzerSettings{});
    const auto statement = TranslateStatement(analyzed.statement(), QueryParameters{},
                                              DefaultDataset{.project = "p", .dataset = "ds"});
    return statement ? std::optional<std::string>(statement->sql) : std::nullopt;
  }

  googlesql::TypeFactory types_;
  TestTableSource source_;
  BigQueryCatalog catalog_{source_, &types_, "p", "ds"};
};

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

TEST_F(TranslatorTest, SessionizeBindsItsInputOnce) {
  const auto sql = Translate(
      "WITH input AS (SELECT RAND() AS marker, RANGE(DATE '2000-01-01', DATE '2000-01-02') AS r) "
      "SELECT * FROM RANGE_SESSIONIZE(TABLE input, 'r', [])");
  ASSERT_TRUE(sql.has_value());
  const auto random = sql->find("random()");
  ASSERT_NE(random, std::string::npos);
  EXPECT_EQ(sql->find("random()", random + 1), std::string::npos);
}

}  // namespace
}  // namespace bigquery_emulator_duckdb
