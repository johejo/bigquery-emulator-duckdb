#include "src/translator/functions.h"

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "gtest/gtest.h"

namespace bigquery_emulator_duckdb::translator {
namespace {

FunctionArgument Sql(std::string sql, googlesql::TypeKind type = googlesql::TYPE_UNKNOWN) {
  return {.sql = std::move(sql), .type = type};
}

FunctionArgument Part(const std::string& part) {
  return {.sql = "'" + part + "'", .date_part = part};
}

FunctionArgument Mode(const std::string& mode) {
  return {.sql = "'" + mode + "'", .rounding_mode = mode};
}

TEST(FunctionsTest, MatchesTheArgumentCount) {
  EXPECT_EQ(TranslateFunction("LOG", {Sql("x")}), "ln(x)");
  EXPECT_EQ(TranslateFunction("LOG", {Sql("x"), Sql("b")}), "log(b, x)");
  EXPECT_EQ(TranslateFunction("LOG", {Sql("x"), Sql("b"), Sql("c")}), std::nullopt);
}

TEST(FunctionsTest, MatchesTheArgumentTypes) {
  EXPECT_EQ(TranslateFunction("BYTE_LENGTH", {Sql("s", googlesql::TYPE_STRING)}), "strlen(s)");
  EXPECT_EQ(TranslateFunction("BYTE_LENGTH", {Sql("b", googlesql::TYPE_BYTES)}), "octet_length(b)");
  // A function with rules is unsupported when none matches, rather than passed through.
  EXPECT_EQ(TranslateFunction("BYTE_LENGTH", {Sql("x")}), std::nullopt);
  EXPECT_EQ(TranslateFunction("REGEXP_CONTAINS", {Sql("b", googlesql::TYPE_BYTES), Sql("r")}),
            std::nullopt);
}

TEST(FunctionsTest, FillsInDefaults) {
  EXPECT_EQ(TranslateFunction("SPLIT", {Sql("s", googlesql::TYPE_STRING)}), "split(s, ',')");
  EXPECT_EQ(TranslateFunction("SPLIT", {Sql("s", googlesql::TYPE_STRING), Sql("d")}),
            "split(s, d)");
  EXPECT_EQ(TranslateFunction("LPAD", {Sql("s", googlesql::TYPE_STRING), Sql("n")}),
            "lpad(s, CAST(n AS INTEGER), ' ')");
}

TEST(FunctionsTest, MatchesDateParts) {
  EXPECT_EQ(TranslateFunction("DATETIME_TRUNC", {Sql("d"), Part("month")}),
            "date_trunc('month', d)");
  EXPECT_EQ(TranslateFunction("DATETIME_TRUNC", {Sql("d"), Part("isoweek")}),
            "date_trunc('week', d)");
  EXPECT_EQ(TranslateFunction("DATE_DIFF", {Sql("a"), Sql("b"), Part("hour")}),
            "date_sub('hour', b, a)");
  EXPECT_EQ(TranslateFunction("DATE_DIFF", {Sql("a"), Sql("b"), Part("day")}),
            "date_diff('day', b, a)");
  // #n takes a date part only.
  EXPECT_EQ(TranslateFunction("DATE_DIFF", {Sql("a"), Sql("b"), Sql("c")}), std::nullopt);
  EXPECT_EQ(TranslateFunction("LAST_DAY", {Sql("d")}), "last_day(d)");
  EXPECT_EQ(TranslateFunction("LAST_DAY", {Sql("d"), Part("month")}), "last_day(d)");
  EXPECT_EQ(TranslateFunction("LAST_DAY", {Sql("d"), Part("year")}), std::nullopt);
}

TEST(FunctionsTest, MatchesRoundingModes) {
  EXPECT_EQ(TranslateFunction("ROUND", {Sql("x"), Sql("n"), Mode("ROUND_HALF_AWAY_FROM_ZERO")}),
            "round(x, CAST(n AS INTEGER))");
  // Rounding modes are enum literals, so a string argument does not match.
  EXPECT_EQ(TranslateFunction("ROUND", {Sql("x"), Sql("n"), Sql("'ROUND_HALF_EVEN'")}),
            std::nullopt);
}

TEST(FunctionsTest, MatchesStringLiterals) {
  FunctionArgument exact = Sql("'exact'", googlesql::TYPE_STRING);
  exact.string_literal = "exact";
  EXPECT_EQ(TranslateFunction("PARSE_JSON", {Sql("s")}), "json(s)");
  EXPECT_EQ(TranslateFunction("PARSE_JSON", {Sql("s"), exact}), "json(s)");
  EXPECT_EQ(TranslateFunction("PARSE_JSON", {Sql("s"), Sql("'round'", googlesql::TYPE_STRING)}),
            std::nullopt);
}

TEST(FunctionsTest, BindsArgumentsUsedTwice) {
  // Columns and literals are repeated as they are.
  EXPECT_EQ(TranslateFunction("CHR", {Sql("q.a")}),
            "CASE WHEN q.a = 0 THEN '' ELSE chr(CAST(q.a AS INTEGER)) END");
  EXPECT_EQ(TranslateFunction("CHR", {Sql("65")}),
            "CASE WHEN 65 = 0 THEN '' ELSE chr(CAST(65 AS INTEGER)) END");
  EXPECT_EQ(TranslateFunction("CHR", {Sql("CAST(-65 AS BIGINT)")}),
            "CASE WHEN CAST(-65 AS BIGINT) = 0 THEN '' ELSE chr(CAST(CAST(-65 AS BIGINT) AS "
            "INTEGER)) END");
  // Anything else is evaluated once, while trivial arguments stay as they are.
  EXPECT_EQ(TranslateFunction("LEFT", {Sql("q.s", googlesql::TYPE_STRING), Sql("(q.n + 1)")}),
            "list_transform([struct_pack(a2 := (q.n + 1))], _fn -> CASE WHEN _fn.a2 < "
            "0 THEN error('LEFT length must be non-negative') ELSE left(q.s, _fn.a2) END)[1]");
}

TEST(FunctionsTest, RaisesErrorsOrNullUnderSafe) {
  EXPECT_EQ(TranslateFunction("LEFT", {Sql("s", googlesql::TYPE_STRING), Sql("n")}),
            "CASE WHEN n < 0 THEN error('LEFT length must be non-negative') ELSE left(s, n) END");
  EXPECT_EQ(TranslateFunction("LEFT", {Sql("s", googlesql::TYPE_STRING), Sql("n")}, true),
            "CASE WHEN n < 0 THEN NULL ELSE left(s, n) END");
  // An error message can use the arguments, which count towards binding them once.
  EXPECT_EQ(TranslateFunction("IPV4_FROM_INT64", {Sql("(q.a + 1)", googlesql::TYPE_INT64)}, true),
            "list_transform([struct_pack(a1 := (q.a + 1))], _fn -> CASE WHEN _fn.a1 < -2147483648 "
            "OR _fn.a1 > 4294967295 THEN NULL ELSE unhex(lpad(hex(_fn.a1 & 4294967295), 8, '0')) "
            "END)[1]");
  EXPECT_EQ(TranslateFunction("ERROR", {Sql("m")}), "error(m)");
  EXPECT_EQ(TranslateFunction("ERROR", {Sql("m")}, true), "NULL");
}

TEST(FunctionsTest, PassesThroughStringsOnly) {
  EXPECT_EQ(TranslateFunction("TRIM", {Sql("s", googlesql::TYPE_STRING)}), "trim(s)");
  EXPECT_EQ(TranslateFunction("TRIM", {Sql("s", googlesql::TYPE_STRING), Sql("c")}), "trim(s, c)");
  EXPECT_EQ(TranslateFunction("TRIM", {Sql("b", googlesql::TYPE_BYTES)}), std::nullopt);
}

TEST(FunctionsTest, RenamesAndPassesThrough) {
  EXPECT_EQ(TranslateFunction("RAND", {}), "random()");
  EXPECT_EQ(TranslateFunction("GREATEST", {Sql("a"), Sql("b"), Sql("c")}), "GREATEST(a, b, c)");
  EXPECT_EQ(TranslateFunction("NO_SUCH_FUNCTION", {Sql("a")}), std::nullopt);
}

TEST(FunctionsTest, RegistersEachFunctionWithItsImplementation) {
  EXPECT_EQ(FindFunction("GREATEST")->implementation, Implementation::kSame);
  EXPECT_EQ(FindFunction("RAND")->implementation, Implementation::kRenamed);
  EXPECT_EQ(FindFunction("LEFT")->implementation, Implementation::kRules);
  EXPECT_EQ(FindFunction("SHA512")->implementation, Implementation::kBackend);
  EXPECT_EQ(FindFunction("JSON_QUERY")->implementation, Implementation::kHandler);
  EXPECT_EQ(FindFunction("SAFE_ADD")->implementation, Implementation::kSafe);
  EXPECT_EQ(FindFunction("SUM")->implementation, Implementation::kAggregate);
  EXPECT_EQ(FindFunction("ROW_NUMBER")->implementation, Implementation::kAnalytic);
  EXPECT_EQ(FindFunction("NO_SUCH_FUNCTION"), nullptr);
  // Only the scalar implementations translate here.
  EXPECT_EQ(TranslateFunction("SUM", {Sql("x")}), std::nullopt);
}

TEST(FunctionsTest, SpellsAggregateArguments) {
  const auto& bytes = FindFunction("STRING_AGG")->aggregates.at(1);
  EXPECT_EQ(AggregateArguments(bytes, {"b"}),
            (std::vector<std::string>{"hex(b)", "hex(unhex('2C'))"}));
  EXPECT_EQ(AggregateArguments(bytes, {"b", "d"}), (std::vector<std::string>{"hex(b)", "hex(d)"}));
  EXPECT_EQ(AggregateArguments(FindFunction("SUM")->aggregates.at(0), {"x"}),
            std::vector<std::string>{"x"});
}

}  // namespace
}  // namespace bigquery_emulator_duckdb::translator
