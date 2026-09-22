#include "src/translator.h"

#include <string>

#include "gtest/gtest.h"
#include "src/frontend.h"

namespace bigquery_emulator_duckdb {
namespace {

std::string Translate(const std::string& sql) { return TranslateToDuckDbSql(ParseGoogleSql(sql)); }

TEST(TranslatorTest, PassesPlainSelectThrough) {
  EXPECT_EQ(Translate("SELECT 1"), "SELECT 1");
  EXPECT_EQ(Translate("SELECT a, b FROM t WHERE a > 1 ORDER BY b"),
            "SELECT a, b FROM t WHERE a > 1 ORDER BY b");
}

TEST(TranslatorTest, ConvertsBacktickIdentifiers) {
  EXPECT_EQ(Translate("SELECT * FROM `project.dataset.table`"),
            "SELECT * FROM \"project\".\"dataset\".\"table\"");
  EXPECT_EQ(Translate("SELECT * FROM `my-project`.dataset.`table`"),
            "SELECT * FROM \"my-project\".dataset.\"table\"");
  EXPECT_EQ(Translate("SELECT `select` FROM t"), "SELECT \"select\" FROM t");
}

TEST(TranslatorTest, ConvertsStringLiterals) {
  EXPECT_EQ(Translate("SELECT 'abc'"), "SELECT 'abc'");
  EXPECT_EQ(Translate("SELECT \"abc\""), "SELECT 'abc'");
  EXPECT_EQ(Translate("SELECT \"it's\""), "SELECT 'it''s'");
  EXPECT_EQ(Translate("SELECT 'a\\nb'"), "SELECT E'a\\nb'");
  EXPECT_EQ(Translate("SELECT r'a\\nb'"), "SELECT 'a\\nb'");
  EXPECT_EQ(Translate("SELECT b'abc'"), "SELECT 'abc'::BLOB");
  EXPECT_EQ(Translate("SELECT '''multi\nline'''"), "SELECT 'multi\nline'");
  EXPECT_EQ(Translate("SELECT \"\"\"quoted \"x\" here\"\"\""), "SELECT 'quoted \"x\" here'");
}

TEST(TranslatorTest, ConvertsTypeNames) {
  EXPECT_EQ(Translate("SELECT CAST(x AS INT64) FROM t"), "SELECT CAST(x AS BIGINT) FROM t");
  EXPECT_EQ(Translate("SELECT CAST(x AS STRING) FROM t"), "SELECT CAST(x AS VARCHAR) FROM t");
  EXPECT_EQ(Translate("SELECT CAST(x AS TIMESTAMP) FROM t"),
            "SELECT CAST(x AS TIMESTAMPTZ) FROM t");
  EXPECT_EQ(Translate("SELECT CAST(x AS DATETIME) FROM t"), "SELECT CAST(x AS TIMESTAMP) FROM t");
  EXPECT_EQ(Translate("SELECT TIMESTAMP '2020-01-01 00:00:00'"),
            "SELECT TIMESTAMPTZ '2020-01-01 00:00:00'");
  EXPECT_EQ(Translate("CREATE TABLE d.t (id INT64, name STRING, flag BOOL, price NUMERIC)"),
            "CREATE TABLE d.t (id BIGINT, name VARCHAR, flag BOOLEAN, price DECIMAL(38, 9))");
}

TEST(TranslatorTest, KeepsTypeNamedFunctions) {
  EXPECT_EQ(Translate("SELECT STRING(x) FROM t"), "SELECT STRING(x) FROM t");
  EXPECT_EQ(Translate("SELECT TIMESTAMP(x) FROM t"), "SELECT TIMESTAMP(x) FROM t");
}

TEST(TranslatorTest, ConvertsFloatLiterals) {
  EXPECT_EQ(Translate("SELECT 1.5, 2, .5, 1e3, 1.5e-3, -0.25"),
            "SELECT 1.5::DOUBLE, 2, .5::DOUBLE, 1e3, 1.5e-3, -0.25::DOUBLE");
  EXPECT_EQ(Translate("SELECT x FROM t WHERE t.x = 1"), "SELECT x FROM t WHERE t.x = 1");
}

TEST(TranslatorTest, ConvertsFunctionNames) {
  EXPECT_EQ(Translate("SELECT SAFE_CAST('x' AS INT64)"), "SELECT TRY_CAST('x' AS BIGINT)");
}

TEST(TranslatorTest, ConvertsCurrentTimeFunctions) {
  EXPECT_EQ(Translate("SELECT CURRENT_TIMESTAMP(), CURRENT_DATE(), current_time()"),
            "SELECT CURRENT_TIMESTAMP, CURRENT_DATE, current_time");
  EXPECT_EQ(Translate("SELECT CURRENT_DATE"), "SELECT CURRENT_DATE");
}

TEST(TranslatorTest, ConvertsStructConstructors) {
  EXPECT_EQ(Translate("SELECT STRUCT(1 AS a, 'x' AS b)"), "SELECT struct_pack(a := 1, b := 'x')");
  EXPECT_EQ(Translate("SELECT STRUCT(1, 2)"), "SELECT struct_pack(_field_1 := 1, _field_2 := 2)");
  EXPECT_EQ(Translate("SELECT STRUCT(STRUCT(1 AS x) AS nested, f(a, b) AS c) AS s FROM t"),
            "SELECT struct_pack(nested := struct_pack(x := 1), c := f(a, b)) AS s FROM t");
  EXPECT_EQ(Translate("SELECT STRUCT(CAST(1 AS INT64) AS n, [1, 2] AS arr)"),
            "SELECT struct_pack(n := CAST(1 AS BIGINT), arr := [1, 2])");
  EXPECT_EQ(Translate("SELECT STRUCT<a INT64, b STRING>(1, 'x')"),
            "SELECT struct_pack(_field_1 := 1, _field_2 := 'x')");
}

TEST(TranslatorTest, DropsArrayTypeParameters) {
  EXPECT_EQ(Translate("SELECT ARRAY<INT64>[1, 2], ARRAY<STRUCT<a INT64>>[]"), "SELECT [1, 2], []");
}

TEST(TranslatorTest, ConvertsComments) {
  EXPECT_EQ(Translate("SELECT 1 # comment"), "SELECT 1 -- comment");
  EXPECT_EQ(Translate("SELECT 1 -- comment"), "SELECT 1 -- comment");
  EXPECT_EQ(Translate("SELECT /* 'x' */ 1"), "SELECT /* 'x' */ 1");
}

TEST(TranslatorTest, RejectsInvalidSyntax) {
  EXPECT_THROW(Translate("SELECT FROM WHERE"), std::runtime_error);
}

}  // namespace
}  // namespace bigquery_emulator_duckdb
