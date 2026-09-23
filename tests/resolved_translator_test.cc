#include "src/resolved_translator.h"

#include <exception>
#include <optional>
#include <string>
#include <vector>

#include "googlesql/public/type.h"
#include "gtest/gtest.h"
#include "nlohmann/json.hpp"
#include "src/analyzer.h"
#include "src/catalog.h"
#include "src/frontend.h"

namespace bigquery_emulator_duckdb {
namespace {

class EmptyTableSource : public TableSource {
 public:
  std::optional<std::vector<FieldSchema>> FindTable(const std::string&, const std::string&,
                                                    const std::string&) override {
    return std::nullopt;
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

  googlesql::TypeFactory types_;
  EmptyTableSource source_;
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
  EXPECT_EQ(sql.value(),
            "SELECT CAST(1 AS BIGINT) AS \"z\", CAST(2.5 AS DOUBLE) AS \"a\", "
            "CAST(NULL AS BIGINT) AS \"n\", CAST(true AS BOOLEAN) AS \"b\", "
            "CAST(from_hex('00ff') AS BLOB) AS \"raw\"");
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
  EXPECT_EQ(sql.value(),
            "SELECT CAST(1 AS BIGINT) AS \"same\", CAST(2 AS BIGINT) AS \"same\", "
            "CAST(3 AS BIGINT) AS \"a.b\"");
}

TEST_F(ResolvedTranslatorTest, FallsBackForUnsupportedConstructs) {
  for (const auto* sql :
       {"SELECT 1 + 2", "SELECT LENGTH('abc')", "SELECT [1, 2]",
        "SELECT x FROM (SELECT 1 AS x) WHERE TRUE", "SELECT SUM(x) FROM (SELECT 1 AS x)",
        "SELECT AS VALUE 1", "SELECT SAFE.BYTE_LENGTH('abc')",
        "SELECT BYTE_LENGTH('abc'), CURRENT_DATE()", "CREATE TABLE ds.t (x INT64)"}) {
    EXPECT_FALSE(Translate(sql).has_value()) << sql;
  }
}

TEST_F(ResolvedTranslatorTest, DoesNotConvertParameterErrorsToFallback) {
  AnalyzerSettings settings;
  settings.named_parameters.emplace_back("s", googlesql::types::StringType());
  EXPECT_THROW(Translate("SELECT BYTE_LENGTH(@s)", {}, settings), std::exception);
}

}  // namespace
}  // namespace bigquery_emulator_duckdb
