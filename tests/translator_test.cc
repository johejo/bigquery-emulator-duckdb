#include "src/translator.h"

#include <cctype>
#include <string>

#include "gtest/gtest.h"
#include "nlohmann/json.hpp"
#include "src/api_error.h"
#include "src/frontend.h"
#include "src/query_parameters.h"

namespace bigquery_emulator_duckdb {
namespace {

std::string Translate(const std::string& sql, const QueryParameters& parameters = {}) {
  return TranslateToDuckDbSql(ParseGoogleSql(sql), parameters);
}

// The translator unparses the AST, so line breaks and indentation are the unparser's rather
// than the input's. Collapsing runs of whitespace keeps the expectations readable; the cases
// that depend on whitespace inside a literal use Translate() and search the output instead.
std::string TranslateOneLine(const std::string& sql, const QueryParameters& parameters = {}) {
  const std::string translated = Translate(sql, parameters);
  std::string result;
  bool in_space = false;
  for (const char c : translated) {
    if (std::isspace(static_cast<unsigned char>(c)) != 0) {
      in_space = true;
      continue;
    }
    if (in_space && !result.empty()) {
      result += ' ';
    }
    in_space = false;
    result += c;
  }
  return result;
}

TEST(TranslatorTest, PassesPlainSelectThrough) {
  EXPECT_EQ(TranslateOneLine("SELECT 1"), "SELECT 1");
  EXPECT_EQ(TranslateOneLine("SELECT a, b FROM t WHERE a > 1 ORDER BY b"),
            "SELECT a, b FROM t WHERE a > 1 ORDER BY b");
  EXPECT_EQ(TranslateOneLine("WITH c AS (SELECT 1 AS x) SELECT x FROM c JOIN t USING (x)"),
            "WITH c AS ( SELECT 1 AS x ) SELECT x FROM c JOIN t USING(x)");
}

TEST(TranslatorTest, ConvertsBacktickIdentifiers) {
  EXPECT_EQ(TranslateOneLine("SELECT * FROM `project.dataset.table`"),
            "SELECT * FROM \"project\".\"dataset\".\"table\"");
  EXPECT_EQ(TranslateOneLine("SELECT * FROM `my-project`.dataset.`table`"),
            "SELECT * FROM \"my-project\".dataset.\"table\"");
  EXPECT_EQ(TranslateOneLine("SELECT `select` FROM t"), "SELECT \"select\" FROM t");
}

TEST(TranslatorTest, ConvertsStringLiterals) {
  EXPECT_EQ(TranslateOneLine("SELECT 'abc'"), "SELECT 'abc'");
  EXPECT_EQ(TranslateOneLine("SELECT \"abc\""), "SELECT 'abc'");
  EXPECT_EQ(TranslateOneLine("SELECT \"it's\""), "SELECT 'it''s'");
  EXPECT_EQ(TranslateOneLine("SELECT \"\"\"quoted \"x\" here\"\"\""), "SELECT 'quoted \"x\" here'");
  // The parser unescapes the literal, so the escape reaches DuckDB as a real newline, whereas
  // a raw literal keeps its backslash.
  EXPECT_NE(Translate("SELECT 'a\\nb'").find("'a\nb'"), std::string::npos);
  EXPECT_EQ(TranslateOneLine("SELECT r'a\\nb'"), "SELECT 'a\\nb'");
  EXPECT_NE(Translate("SELECT '''multi\nline'''").find("'multi\nline'"), std::string::npos);
}

TEST(TranslatorTest, ConvertsBytesLiterals) {
  // Bytes may hold quotes, NUL and non-UTF-8 sequences, so they travel as hex.
  EXPECT_EQ(TranslateOneLine("SELECT b'abc'"), "SELECT from_hex('616263')");
  EXPECT_EQ(TranslateOneLine("SELECT b'\\x00\\xff'"), "SELECT from_hex('00ff')");
}

TEST(TranslatorTest, ConvertsTypeNames) {
  EXPECT_EQ(TranslateOneLine("SELECT CAST(x AS INT64) FROM t"), "SELECT CAST(x AS BIGINT) FROM t");
  EXPECT_EQ(TranslateOneLine("SELECT CAST(x AS STRING) FROM t"),
            "SELECT CAST(x AS VARCHAR) FROM t");
  EXPECT_EQ(TranslateOneLine("SELECT CAST(x AS TIMESTAMP) FROM t"),
            "SELECT CAST(x AS TIMESTAMPTZ) FROM t");
  EXPECT_EQ(TranslateOneLine("SELECT CAST(x AS DATETIME) FROM t"),
            "SELECT CAST(x AS TIMESTAMP) FROM t");
  EXPECT_EQ(TranslateOneLine("SELECT CAST(x AS ARRAY<INT64>) FROM t"),
            "SELECT CAST(x AS BIGINT[]) FROM t");
  EXPECT_EQ(TranslateOneLine("SELECT CAST(x AS STRUCT<a INT64, b STRING>) FROM t"),
            "SELECT CAST(x AS STRUCT(a BIGINT, b VARCHAR)) FROM t");
  EXPECT_EQ(TranslateOneLine("SELECT CAST(x AS NUMERIC) FROM t"),
            "SELECT CAST(x AS DECIMAL(38, 9)) FROM t");
}

TEST(TranslatorTest, ConvertsDateAndTimeLiterals) {
  EXPECT_EQ(TranslateOneLine("SELECT TIMESTAMP '2020-01-01 00:00:00'"),
            "SELECT TIMESTAMPTZ '2020-01-01 00:00:00'");
  EXPECT_EQ(TranslateOneLine("SELECT DATETIME '2020-01-01 00:00:00'"),
            "SELECT TIMESTAMP '2020-01-01 00:00:00'");
  EXPECT_EQ(TranslateOneLine("SELECT DATE '2020-01-01'"), "SELECT DATE '2020-01-01'");
}

TEST(TranslatorTest, ConvertsColumnTypesInDdl) {
  EXPECT_EQ(TranslateOneLine("CREATE TABLE d.t (id INT64, name STRING, flag BOOL, price NUMERIC)"),
            "CREATE TABLE d.t ( id BIGINT, name VARCHAR, flag BOOLEAN, price DECIMAL(38, 9) )");
  EXPECT_EQ(TranslateOneLine("CREATE TABLE d.t (tags ARRAY<STRING>, point STRUCT<x INT64>)"),
            "CREATE TABLE d.t ( tags VARCHAR[], point STRUCT(x BIGINT) )");
}

TEST(TranslatorTest, KeepsTypeNamedFunctions) {
  EXPECT_EQ(TranslateOneLine("SELECT STRING(x) FROM t"), "SELECT STRING(x) FROM t");
  EXPECT_EQ(TranslateOneLine("SELECT TIMESTAMP(x) FROM t"), "SELECT TIMESTAMP(x) FROM t");
}

TEST(TranslatorTest, ConvertsFloatLiterals) {
  EXPECT_EQ(TranslateOneLine("SELECT 1.5, 2, .5, 1e3, 1.5e-3, -0.25"),
            "SELECT 1.5::DOUBLE, 2, .5::DOUBLE, 1e3::DOUBLE, 1.5e-3::DOUBLE, -0.25::DOUBLE");
  EXPECT_EQ(TranslateOneLine("SELECT x FROM t WHERE t.x = 1"), "SELECT x FROM t WHERE t.x = 1");
}

TEST(TranslatorTest, ConvertsFunctionNames) {
  EXPECT_EQ(TranslateOneLine("SELECT SAFE_CAST('x' AS INT64)"), "SELECT TRY_CAST('x' AS BIGINT)");
}

TEST(TranslatorTest, RenamesFunctions) {
  EXPECT_EQ(TranslateOneLine("SELECT REGEXP_CONTAINS(s, r'a.c') FROM t"),
            "SELECT regexp_matches(s, 'a.c') FROM t");
  EXPECT_EQ(TranslateOneLine("SELECT LOGICAL_AND(a), LOGICAL_OR(b) FROM t"),
            "SELECT bool_and(a), bool_or(b) FROM t");
  EXPECT_EQ(TranslateOneLine("SELECT DIV(7, 2), IS_NAN(x), IS_INF(x) FROM t"),
            "SELECT divide(7, 2), isnan(x), isinf(x) FROM t");
  EXPECT_EQ(TranslateOneLine("SELECT FORMAT('%s-%d', s, n) FROM t"),
            "SELECT printf('%s-%d', s, n) FROM t");
  EXPECT_EQ(TranslateOneLine("SELECT GENERATE_UUID(), RAND()"), "SELECT uuid(), random()");
  EXPECT_EQ(TranslateOneLine("SELECT GENERATE_ARRAY(1, 5, 2)"), "SELECT generate_series(1, 5, 2)");
  EXPECT_EQ(TranslateOneLine("SELECT JSON_QUERY(j, '$.a'), JSON_EXTRACT_SCALAR(j, '$.b') FROM t"),
            "SELECT json_extract(j, '$.a'), json_extract_string(j, '$.b') FROM t");
}

// A rename replaces the name and nothing else, so everything the base unparser prints around
// the arguments survives.
TEST(TranslatorTest, KeepsModifiersOnRenamedFunctions) {
  EXPECT_EQ(TranslateOneLine("SELECT LOGICAL_AND(DISTINCT a) FROM t"),
            "SELECT bool_and(DISTINCT a) FROM t");
  EXPECT_EQ(TranslateOneLine("SELECT LOGICAL_OR(b) OVER (PARTITION BY a) FROM t"),
            "SELECT bool_or(b) OVER (PARTITION BY a) FROM t");
}

TEST(TranslatorTest, RewritesDateAndTimeArithmetic) {
  // DATE_ADD stays a DATE, which the DuckDB operator would widen to a TIMESTAMP.
  EXPECT_EQ(TranslateOneLine("SELECT DATE_ADD(d, INTERVAL 1 DAY) FROM t"),
            "SELECT CAST(d + INTERVAL 1 DAY AS DATE) FROM t");
  EXPECT_EQ(TranslateOneLine("SELECT DATE_SUB(d, INTERVAL n MONTH) FROM t"),
            "SELECT CAST(d - INTERVAL n MONTH AS DATE) FROM t");
  EXPECT_EQ(TranslateOneLine("SELECT TIMESTAMP_ADD(ts, INTERVAL 1 HOUR) FROM t"),
            "SELECT (ts + INTERVAL 1 HOUR) FROM t");
  EXPECT_EQ(TranslateOneLine("SELECT DATETIME_SUB(dt, INTERVAL 1 MINUTE) FROM t"),
            "SELECT (dt - INTERVAL 1 MINUTE) FROM t");
}

// The date part is a keyword in BigQuery and a string in DuckDB, and the arguments of DIFF are
// the other way round.
TEST(TranslatorTest, RewritesDatePartFunctions) {
  EXPECT_EQ(TranslateOneLine("SELECT DATE_DIFF(a, b, DAY) FROM t"),
            "SELECT date_diff('day', b, a) FROM t");
  EXPECT_EQ(TranslateOneLine("SELECT TIMESTAMP_DIFF(a, b, SECOND) FROM t"),
            "SELECT date_diff('second', b, a) FROM t");
  EXPECT_EQ(TranslateOneLine("SELECT DATE_TRUNC(d, MONTH) FROM t"),
            "SELECT CAST(date_trunc('month', d) AS DATE) FROM t");
  EXPECT_EQ(TranslateOneLine("SELECT TIMESTAMP_TRUNC(ts, HOUR) FROM t"),
            "SELECT date_trunc('hour', ts) FROM t");
}

TEST(TranslatorTest, RewritesFormattingAndParsing) {
  EXPECT_EQ(TranslateOneLine("SELECT FORMAT_TIMESTAMP('%Y-%m-%d', ts) FROM t"),
            "SELECT strftime(ts, '%Y-%m-%d') FROM t");
  EXPECT_EQ(TranslateOneLine("SELECT FORMAT_DATE('%Y', d) FROM t"),
            "SELECT strftime(d, '%Y') FROM t");
  EXPECT_EQ(TranslateOneLine("SELECT PARSE_DATE('%Y-%m-%d', s) FROM t"),
            "SELECT CAST(strptime(s, '%Y-%m-%d') AS DATE) FROM t");
  EXPECT_EQ(TranslateOneLine("SELECT PARSE_TIMESTAMP('%Y-%m-%d', s) FROM t"),
            "SELECT CAST(strptime(s, '%Y-%m-%d') AS TIMESTAMPTZ) FROM t");
}

TEST(TranslatorTest, RewritesEpochConversions) {
  EXPECT_EQ(TranslateOneLine("SELECT UNIX_SECONDS(ts) FROM t"),
            "SELECT CAST(epoch(ts) AS BIGINT) FROM t");
  EXPECT_EQ(TranslateOneLine("SELECT UNIX_MILLIS(ts), UNIX_MICROS(ts) FROM t"),
            "SELECT epoch_ms(ts), epoch_us(ts) FROM t");
  EXPECT_EQ(TranslateOneLine("SELECT TIMESTAMP_SECONDS(n) FROM t"),
            "SELECT to_timestamp(n) FROM t");
  EXPECT_EQ(TranslateOneLine("SELECT TIMESTAMP_MILLIS(n) FROM t"),
            "SELECT (epoch_ms(n) AT TIME ZONE 'UTC') FROM t");
}

TEST(TranslatorTest, RewritesCallsWithDifferentSemantics) {
  // A division by zero is an error in BigQuery and +Inf in DuckDB.
  EXPECT_EQ(TranslateOneLine("SELECT SAFE_DIVIDE(a, b) FROM t"),
            "SELECT (a / NULLIF(b, 0)) FROM t");
  // DuckDB's log() is the base 10 logarithm and takes the base first.
  EXPECT_EQ(TranslateOneLine("SELECT LOG(x), LOG(x, 2) FROM t"), "SELECT ln(x), log(2, x) FROM t");
  // REGEXP_REPLACE replaces every occurrence, DuckDB only the first without the flag.
  EXPECT_EQ(TranslateOneLine("SELECT REGEXP_REPLACE(s, 'a', 'b') FROM t"),
            "SELECT regexp_replace(s, 'a', 'b', 'g') FROM t");
  // Only the one argument form needs a rule; DuckDB matches the name case insensitively.
  EXPECT_EQ(TranslateOneLine("SELECT SPLIT(s), SPLIT(s, ';') FROM t"),
            "SELECT split(s, ','), SPLIT(s, ';') FROM t");
  EXPECT_EQ(TranslateOneLine("SELECT TO_JSON_STRING(x) FROM t"),
            "SELECT CAST(to_json(x) AS VARCHAR) FROM t");
}

// A rewrite turns the call into an expression, which an OVER clause could no longer attach to,
// so the call carrying the OVER keeps its BigQuery spelling. A call inside the window spec is
// rewritten as usual.
TEST(TranslatorTest, LeavesRewrittenCallsUnderOverAlone) {
  EXPECT_EQ(TranslateOneLine("SELECT SUM(x) OVER (ORDER BY DATE_TRUNC(d, DAY)) FROM t"),
            "SELECT SUM(x) OVER ( ORDER BY CAST(date_trunc('day', d) AS DATE)) FROM t");
}

// DuckDB has no call that swallows errors, so the prefix goes and the error propagates.
TEST(TranslatorTest, DropsTheSafePrefix) {
  EXPECT_EQ(TranslateOneLine("SELECT SAFE.SUBSTR(s, 1, 2) FROM t"),
            "SELECT SUBSTR(s, 1, 2) FROM t");
  EXPECT_EQ(TranslateOneLine("SELECT SAFE.REGEXP_CONTAINS(s, 'a') FROM t"),
            "SELECT regexp_matches(s, 'a') FROM t");
}

// A call the mapping does not cover keeps its arguments and its name, including a call whose
// name a rule matches but whose argument count it does not.
TEST(TranslatorTest, LeavesUnmappedFunctionsAlone) {
  EXPECT_EQ(TranslateOneLine("SELECT COALESCE(a, b), IFNULL(a, b), IF(a, b, c) FROM t"),
            "SELECT COALESCE(a, b), IFNULL(a, b), IF(a, b, c) FROM t");
  EXPECT_EQ(TranslateOneLine("SELECT STRING_AGG(DISTINCT s, ',' ORDER BY s) FROM t"),
            "SELECT STRING_AGG(DISTINCT s, ',' ORDER BY s) FROM t");
  // FORMAT_TIMESTAMP with a time zone is not mapped.
  EXPECT_EQ(TranslateOneLine("SELECT FORMAT_TIMESTAMP('%Y', ts, 'UTC') FROM t"),
            "SELECT FORMAT_TIMESTAMP('%Y', ts, 'UTC') FROM t");
}

TEST(TranslatorTest, ConvertsStarModifiers) {
  EXPECT_EQ(TranslateOneLine("SELECT * EXCEPT (a, b) FROM t"), "SELECT * EXCLUDE (a, b) FROM t");
  EXPECT_EQ(TranslateOneLine("SELECT * REPLACE (a * 2 AS a) FROM t"),
            "SELECT * REPLACE (a * 2 AS a) FROM t");
  EXPECT_EQ(TranslateOneLine("SELECT t.* EXCEPT (a) FROM t"), "SELECT t.* EXCLUDE (a) FROM t");
}

// QUALIFY, window functions and the set operators are spelled the same in both dialects.
TEST(TranslatorTest, PassesQualifyThrough) {
  EXPECT_EQ(TranslateOneLine("SELECT a FROM t QUALIFY ROW_NUMBER() OVER (ORDER BY a) = 1"),
            "SELECT a FROM t QUALIFY ROW_NUMBER() OVER ( ORDER BY a) = 1");
}

TEST(TranslatorTest, ConvertsCurrentTimeFunctions) {
  EXPECT_EQ(TranslateOneLine("SELECT CURRENT_TIMESTAMP(), CURRENT_DATE(), current_time()"),
            "SELECT CURRENT_TIMESTAMP, CURRENT_DATE, current_time");
  EXPECT_EQ(TranslateOneLine("SELECT CURRENT_DATE"), "SELECT CURRENT_DATE");
}

TEST(TranslatorTest, ConvertsStructConstructors) {
  EXPECT_EQ(TranslateOneLine("SELECT STRUCT(1 AS a, 'x' AS b)"),
            "SELECT struct_pack(a := 1, b := 'x')");
  EXPECT_EQ(TranslateOneLine("SELECT STRUCT(1, 2)"),
            "SELECT struct_pack(_field_1 := 1, _field_2 := 2)");
  EXPECT_EQ(TranslateOneLine("SELECT STRUCT(STRUCT(1 AS x) AS nested, f(a, b) AS c) AS s FROM t"),
            "SELECT struct_pack(nested := struct_pack(x := 1), c := f(a, b)) AS s FROM t");
  EXPECT_EQ(TranslateOneLine("SELECT STRUCT(CAST(1 AS INT64) AS n, [1, 2] AS arr)"),
            "SELECT struct_pack(n := CAST(1 AS BIGINT), arr := [1, 2])");
  EXPECT_EQ(TranslateOneLine("SELECT STRUCT<a INT64, b STRING>(1, 'x')"),
            "SELECT struct_pack(_field_1 := 1, _field_2 := 'x')");
}

TEST(TranslatorTest, DropsArrayTypeParameters) {
  EXPECT_EQ(TranslateOneLine("SELECT ARRAY<INT64>[1, 2], ARRAY<STRUCT<a INT64>>[]"),
            "SELECT [1, 2], []");
}

TEST(TranslatorTest, DropsComments) {
  // Comments are not part of the AST, so unparsing loses them.
  EXPECT_EQ(TranslateOneLine("SELECT 1 # comment"), "SELECT 1");
  EXPECT_EQ(TranslateOneLine("SELECT 1 -- comment"), "SELECT 1");
  EXPECT_EQ(TranslateOneLine("SELECT /* 'x' */ 1"), "SELECT 1");
}

TEST(TranslatorTest, RejectsInvalidSyntax) {
  EXPECT_THROW(Translate("SELECT FROM WHERE"), std::runtime_error);
}

TEST(TranslatorTest, ReplacesNamedQueryParameters) {
  const QueryParameters parameters = QueryParameters::Parse(nlohmann::json::parse(R"([
      {"name": "id", "parameterType": {"type": "INT64"}, "parameterValue": {"value": "42"}},
      {"name": "name", "parameterType": {"type": "STRING"},
       "parameterValue": {"value": "a'b"}}])"));
  EXPECT_EQ(TranslateOneLine("SELECT @name FROM t WHERE id = @id", parameters),
            "SELECT CAST('a''b' AS VARCHAR) FROM t WHERE id = CAST('42' AS BIGINT)");
}

TEST(TranslatorTest, ReplacesPositionalQueryParameters) {
  const QueryParameters parameters = QueryParameters::Parse(nlohmann::json::parse(R"([
      {"parameterType": {"type": "INT64"}, "parameterValue": {"value": "1"}},
      {"parameterType": {"type": "STRING"}, "parameterValue": {"value": "x"}}])"));
  EXPECT_EQ(TranslateOneLine("SELECT * FROM t WHERE a = ? AND b = ?", parameters),
            "SELECT * FROM t WHERE a = CAST('1' AS BIGINT) AND b = CAST('x' AS VARCHAR)");
}

TEST(TranslatorTest, RejectsUndeclaredQueryParameters) {
  EXPECT_THROW(Translate("SELECT @missing"), ApiError);
}

}  // namespace
}  // namespace bigquery_emulator_duckdb
