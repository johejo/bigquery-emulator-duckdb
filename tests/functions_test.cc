#include "src/functions.h"

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "gtest/gtest.h"

namespace bigquery_emulator_duckdb {
namespace {

FunctionArgument Sql(std::string sql, ArgumentType type = ArgumentType::kOther) {
  return {.sql = std::move(sql), .type = type};
}

FunctionArgument Part(const std::string& part) {
  return {.sql = "'" + part + "'", .date_part = part};
}

TEST(FunctionsTest, MatchesTheArgumentCount) {
  EXPECT_EQ(TranslateFunction("LOG", {Sql("x")}), "ln(x)");
  EXPECT_EQ(TranslateFunction("LOG", {Sql("x"), Sql("b")}), "log(b, x)");
  EXPECT_EQ(TranslateFunction("LOG", {Sql("x"), Sql("b"), Sql("c")}), std::nullopt);
}

TEST(FunctionsTest, MatchesTheArgumentTypes) {
  EXPECT_EQ(TranslateFunction("BYTE_LENGTH", {Sql("s", ArgumentType::kString)}), "strlen(s)");
  EXPECT_EQ(TranslateFunction("BYTE_LENGTH", {Sql("b", ArgumentType::kBytes)}), "octet_length(b)");
  // A function with rules is unsupported when none matches, rather than passed through.
  EXPECT_EQ(TranslateFunction("BYTE_LENGTH", {Sql("x")}), std::nullopt);
  EXPECT_EQ(TranslateFunction("TRANSLATE", {Sql("b", ArgumentType::kBytes), Sql("c"), Sql("d")}),
            std::nullopt);
}

TEST(FunctionsTest, FillsInDefaults) {
  EXPECT_EQ(TranslateFunction("SPLIT", {Sql("s", ArgumentType::kString)}), "split(s, ',')");
  EXPECT_EQ(TranslateFunction("SPLIT", {Sql("s", ArgumentType::kString), Sql("d")}), "split(s, d)");
  EXPECT_EQ(TranslateFunction("LPAD", {Sql("s", ArgumentType::kString), Sql("n")}),
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

TEST(FunctionsTest, MatchesStringLiterals) {
  FunctionArgument exact = Sql("'exact'", ArgumentType::kString);
  exact.string_literal = "exact";
  EXPECT_EQ(TranslateFunction("PARSE_JSON", {Sql("s")}), "json(s)");
  EXPECT_EQ(TranslateFunction("PARSE_JSON", {Sql("s"), exact}), "json(s)");
  EXPECT_EQ(TranslateFunction("PARSE_JSON", {Sql("s"), Sql("'round'", ArgumentType::kString)}),
            std::nullopt);
}

TEST(FunctionsTest, BindsArgumentsUsedTwice) {
  // Columns and literals are repeated as they are.
  EXPECT_EQ(TranslateFunction("CHR", {Sql("q.a")}),
            "CASE WHEN q.a = 0 THEN '' ELSE chr(CAST(q.a AS INTEGER)) END");
  EXPECT_EQ(TranslateFunction("CHR", {Sql("65")}),
            "CASE WHEN 65 = 0 THEN '' ELSE chr(CAST(65 AS INTEGER)) END");
  // Anything else is evaluated once, with the other arguments bound alongside.
  EXPECT_EQ(TranslateFunction("LEFT", {Sql("q.s", ArgumentType::kString), Sql("(q.n + 1)")}),
            "list_transform([struct_pack(a1 := q.s, a2 := (q.n + 1))], _fn -> CASE WHEN _fn.a2 < "
            "0 THEN error('LEFT length must be non-negative') ELSE left(_fn.a1, _fn.a2) END)[1]");
}

TEST(FunctionsTest, PassesThroughStringsOnly) {
  EXPECT_EQ(TranslateFunction("TRIM", {Sql("s", ArgumentType::kString)}), "trim(s)");
  EXPECT_EQ(TranslateFunction("TRIM", {Sql("s", ArgumentType::kString), Sql("c")}), "trim(s, c)");
  EXPECT_EQ(TranslateFunction("TRIM", {Sql("b", ArgumentType::kBytes)}), std::nullopt);
}

TEST(FunctionsTest, RenamesAndPassesThrough) {
  EXPECT_EQ(TranslateFunction("RAND", {}), "random()");
  EXPECT_EQ(TranslateFunction("GREATEST", {Sql("a"), Sql("b"), Sql("c")}), "GREATEST(a, b, c)");
  EXPECT_EQ(TranslateFunction("NO_SUCH_FUNCTION", {Sql("a")}), std::nullopt);
}

}  // namespace
}  // namespace bigquery_emulator_duckdb
