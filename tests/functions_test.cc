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
  EXPECT_EQ(TranslateFunction("TRUNC", {Sql("x")}), "trunc(x)");
  EXPECT_EQ(TranslateFunction("TRUNC", {Sql("x"), Sql("d")}), "trunc(x, CAST(d AS INTEGER))");
  EXPECT_EQ(TranslateFunction("TRUNC", {Sql("x"), Sql("d"), Sql("e")}), std::nullopt);
}

TEST(FunctionsTest, MatchesTheArgumentTypes) {
  EXPECT_EQ(TranslateFunction("BYTE_LENGTH", {Sql("s", googlesql::TYPE_STRING)}), "strlen(s)");
  EXPECT_EQ(TranslateFunction("BYTE_LENGTH", {Sql("b", googlesql::TYPE_BYTES)}), "octet_length(b)");
  // A function with rules is unsupported when none matches, rather than passed through.
  EXPECT_EQ(TranslateFunction("BYTE_LENGTH", {Sql("x")}), std::nullopt);
  EXPECT_EQ(TranslateFunction("UNICODE", {Sql("b", googlesql::TYPE_BYTES)}), std::nullopt);
}

TEST(FunctionsTest, FillsInDefaults) {
  EXPECT_EQ(TranslateFunction("SPLIT", {Sql("s", googlesql::TYPE_STRING)}), "split(s, ',')");
  EXPECT_EQ(TranslateFunction("SPLIT", {Sql("s", googlesql::TYPE_STRING), Sql("d")}),
            "split(s, d)");
  EXPECT_EQ(TranslateFunction("LPAD", {Sql("s", googlesql::TYPE_STRING), Sql("n")}),
            "bq_lpad(s, n, ' ')");
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
  EXPECT_EQ(TranslateFunction("$BITWISE_LEFT_SHIFT",
                              {Sql("q.a", googlesql::TYPE_INT64), Sql("(q.n + 1)")}),
            "list_transform([struct_pack(a2 := (q.n + 1))], _fn -> CASE WHEN _fn.a2 < 0 THEN "
            "error('Bit shift by a negative value') WHEN _fn.a2 >= 64 THEN 0 ELSE CAST(CAST(q.a "
            "AS BIT) << CAST(_fn.a2 AS INTEGER) AS BIGINT) END)[1]");
}

TEST(FunctionsTest, RaisesErrorsOrNullUnderSafe) {
  EXPECT_EQ(TranslateFunction("SUBSTR", {Sql("s", googlesql::TYPE_STRING), Sql("p"), Sql("n")}),
            "CASE WHEN n < 0 THEN error('Third argument in SUBSTR() cannot be negative') ELSE "
            "substr(s, CASE WHEN p > 0 THEN p WHEN p = 0 OR p < -length(s) THEN 1 ELSE length(s) "
            "+ p + 1 END, n) END");
  EXPECT_EQ(
      TranslateFunction("SUBSTR", {Sql("s", googlesql::TYPE_STRING), Sql("p"), Sql("n")}, true),
      "CASE WHEN n < 0 THEN NULL ELSE substr(s, CASE WHEN p > 0 THEN p WHEN p = 0 OR p < "
      "-length(s) THEN 1 ELSE length(s) + p + 1 END, n) END");
  // An error message can use the arguments, which count towards binding them once.
  EXPECT_EQ(TranslateFunction("IPV4_FROM_INT64", {Sql("(q.a + 1)", googlesql::TYPE_INT64)}, true),
            "list_transform([struct_pack(a1 := (q.a + 1))], _fn -> CASE WHEN _fn.a1 < -2147483648 "
            "OR _fn.a1 > 4294967295 THEN NULL ELSE unhex(lpad(hex(_fn.a1 & 4294967295), 8, '0')) "
            "END)[1]");
  EXPECT_EQ(TranslateFunction("ERROR", {Sql("m")}), "error(m)");
  EXPECT_EQ(TranslateFunction("ERROR", {Sql("m")}, true), "NULL");
}

TEST(FunctionsTest, PassesThroughStringsOnly) {
  EXPECT_EQ(TranslateFunction("FROM_BASE64", {Sql("s", googlesql::TYPE_STRING)}), "from_base64(s)");
  EXPECT_EQ(TranslateFunction("FROM_BASE64", {Sql("b", googlesql::TYPE_BYTES)}), std::nullopt);
}

TEST(FunctionsTest, RenamesAndPassesThrough) {
  EXPECT_EQ(TranslateFunction("RAND", {}), "random()");
  EXPECT_EQ(TranslateFunction("COALESCE", {Sql("a"), Sql("b"), Sql("c")}), "COALESCE(a, b, c)");
  EXPECT_EQ(TranslateFunction("NO_SUCH_FUNCTION", {Sql("a")}), std::nullopt);
}

TEST(FunctionsTest, RegistersEachFunctionWithItsImplementation) {
  EXPECT_EQ(FindFunction("COALESCE")->implementation, Implementation::kSame);
  EXPECT_EQ(FindFunction("RAND")->implementation, Implementation::kRenamed);
  EXPECT_EQ(FindFunction("CHR")->implementation, Implementation::kRules);
  EXPECT_EQ(FindFunction("SHA512")->implementation, Implementation::kBackend);
  EXPECT_EQ(FindFunction("CONCAT")->implementation, Implementation::kHandler);
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
