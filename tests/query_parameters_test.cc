#include "src/query_parameters.h"

#include <string>

#include "gtest/gtest.h"
#include "nlohmann/json.hpp"
#include "src/api_error.h"
#include "src/field_schema.h"

namespace bigquery_emulator_duckdb {
namespace {

using nlohmann::json;

QueryParameters Parse(const std::string& parameters) {
  return QueryParameters::Parse(json::parse(parameters));
}

TEST(QueryParametersTest, ConvertsScalarsToTypedLiterals) {
  const QueryParameters parameters = Parse(R"([
      {"name": "i", "parameterType": {"type": "INT64"}, "parameterValue": {"value": "42"}},
      {"name": "f", "parameterType": {"type": "FLOAT64"}, "parameterValue": {"value": "1.5"}},
      {"name": "b", "parameterType": {"type": "BOOL"}, "parameterValue": {"value": "true"}},
      {"name": "s", "parameterType": {"type": "STRING"}, "parameterValue": {"value": "it's"}},
      {"name": "d", "parameterType": {"type": "DATE"}, "parameterValue": {"value": "2024-01-02"}},
      {"name": "ts", "parameterType": {"type": "TIMESTAMP"},
       "parameterValue": {"value": "2024-01-02 03:04:05+00:00"}},
      {"name": "n", "parameterType": {"type": "NUMERIC"}, "parameterValue": {"value": "1.25"}}])");
  EXPECT_EQ(parameters.ByName("i"), "CAST('42' AS BIGINT)");
  EXPECT_EQ(parameters.ByName("f"), "CAST('1.5' AS DOUBLE)");
  EXPECT_EQ(parameters.ByName("b"), "CAST('true' AS BOOLEAN)");
  EXPECT_EQ(parameters.ByName("s"), "CAST('it''s' AS VARCHAR)");
  EXPECT_EQ(parameters.ByName("d"), "CAST('2024-01-02' AS DATE)");
  EXPECT_EQ(parameters.ByName("ts"), "CAST('2024-01-02 03:04:05+00:00' AS TIMESTAMPTZ)");
  EXPECT_EQ(parameters.ByName("n"), "CAST('1.25' AS DECIMAL(38,9))");
}

TEST(QueryParametersTest, MatchesNamesCaseInsensitively) {
  const QueryParameters parameters = Parse(R"([{"name": "Id", "parameterType": {"type": "INT64"},
                 "parameterValue": {"value": "1"}}])");
  EXPECT_EQ(parameters.ByName("id"), "CAST('1' AS BIGINT)");
  EXPECT_EQ(parameters.ByName("ID"), "CAST('1' AS BIGINT)");
}

TEST(QueryParametersTest, AcceptsLegacyTypeNames) {
  const QueryParameters parameters = Parse(R"([{"name": "i", "parameterType": {"type": "INTEGER"},
                 "parameterValue": {"value": "1"}}])");
  EXPECT_EQ(parameters.ByName("i"), "CAST('1' AS BIGINT)");
}

TEST(QueryParametersTest, DecodesBytesFromBase64) {
  const QueryParameters parameters = Parse(R"([{"name": "b", "parameterType": {"type": "BYTES"},
                 "parameterValue": {"value": "YWJj"}}])");
  EXPECT_EQ(parameters.ByName("b"), "from_base64('YWJj')");
}

TEST(QueryParametersTest, KeepsTheTypeOfNullValues) {
  const QueryParameters parameters = Parse(R"([
      {"name": "missing", "parameterType": {"type": "STRING"}},
      {"name": "null", "parameterType": {"type": "INT64"}, "parameterValue": {"value": null}}])");
  EXPECT_EQ(parameters.ByName("missing"), "CAST(NULL AS VARCHAR)");
  EXPECT_EQ(parameters.ByName("null"), "CAST(NULL AS BIGINT)");
}

TEST(QueryParametersTest, ConvertsArrays) {
  const QueryParameters parameters = Parse(R"([
      {"name": "a", "parameterType": {"type": "ARRAY", "arrayType": {"type": "INT64"}},
       "parameterValue": {"arrayValues": [{"value": "1"}, {"value": "2"}]}},
      {"name": "e", "parameterType": {"type": "ARRAY", "arrayType": {"type": "STRING"}},
       "parameterValue": {"arrayValues": []}}])");
  EXPECT_EQ(parameters.ByName("a"), "CAST([CAST('1' AS BIGINT), CAST('2' AS BIGINT)] AS BIGINT[])");
  EXPECT_EQ(parameters.ByName("e"), "CAST([] AS VARCHAR[])");
}

TEST(QueryParametersTest, ConvertsStructs) {
  const QueryParameters parameters = Parse(R"([
      {"name": "s",
       "parameterType": {"type": "STRUCT", "structTypes": [
          {"name": "x", "type": {"type": "INT64"}},
          {"name": "y", "type": {"type": "STRING"}}]},
       "parameterValue": {"structValues": {"x": {"value": "1"}, "y": {"value": "a"}}}}])");
  EXPECT_EQ(parameters.ByName("s"),
            R"(struct_pack("x" := CAST('1' AS BIGINT), "y" := CAST('a' AS VARCHAR)))");
}

TEST(QueryParametersTest, NumbersParametersWithoutANameByPosition) {
  const QueryParameters parameters = Parse(R"([
      {"parameterType": {"type": "INT64"}, "parameterValue": {"value": "1"}},
      {"parameterType": {"type": "INT64"}, "parameterValue": {"value": "2"}}])");
  EXPECT_EQ(parameters.ByPosition(1), "CAST('1' AS BIGINT)");
  EXPECT_EQ(parameters.ByPosition(2), "CAST('2' AS BIGINT)");
  EXPECT_THROW(static_cast<void>(parameters.ByPosition(3)), ApiError);
}

TEST(QueryParametersTest, KeepsTheDeclaredTypes) {
  const QueryParameters named = Parse(R"([
      {"name": "i", "parameterType": {"type": "INTEGER"}, "parameterValue": {"value": "1"}},
      {"name": "a", "parameterType": {"type": "ARRAY", "arrayType": {"type": "STRING"}},
       "parameterValue": {"arrayValues": []}},
      {"name": "s", "parameterType": {"type": "STRUCT", "structTypes": [
           {"name": "x", "type": {"type": "INT64"}}]},
       "parameterValue": {"structValues": {"x": {"value": "1"}}}}])");
  ASSERT_EQ(named.named_types().size(), 3);
  EXPECT_TRUE(named.positional_types().empty());
  EXPECT_EQ(named.named_types().at(0).name, "i");
  EXPECT_EQ(named.named_types().at(0).type, FieldType::kInteger);
  EXPECT_EQ(named.named_types().at(0).mode, FieldMode::kNullable);
  EXPECT_EQ(named.named_types().at(1).type, FieldType::kString);
  EXPECT_EQ(named.named_types().at(1).mode, FieldMode::kRepeated);
  EXPECT_EQ(named.named_types().at(2).type, FieldType::kRecord);
  ASSERT_EQ(named.named_types().at(2).fields.size(), 1);
  EXPECT_EQ(named.named_types().at(2).fields.at(0).name, "x");
  EXPECT_EQ(named.named_types().at(2).fields.at(0).type, FieldType::kInteger);

  const QueryParameters positional = Parse(R"([
      {"parameterType": {"type": "BOOL"}, "parameterValue": {"value": "true"}}])");
  ASSERT_EQ(positional.positional_types().size(), 1);
  EXPECT_EQ(positional.positional_types().at(0).type, FieldType::kBoolean);
}

TEST(QueryParametersTest, ReportsUndeclaredParameters) {
  const QueryParameters parameters;
  EXPECT_TRUE(parameters.empty());
  EXPECT_THROW(static_cast<void>(parameters.ByName("missing")), ApiError);
  EXPECT_THROW(static_cast<void>(parameters.ByPosition(1)), ApiError);
}

TEST(QueryParametersTest, RejectsMalformedParameters) {
  EXPECT_THROW(Parse(R"({})"), ApiError);
  EXPECT_THROW(Parse(R"([{"name": "a"}])"), ApiError);
  EXPECT_THROW(Parse(R"([{"name": "a", "parameterType": {}}])"), ApiError);
  EXPECT_THROW(Parse(R"([{"name": "a", "parameterType": {"type": "ARRAY", "arrayType":
                 {"type": "ARRAY", "arrayType": {"type": "INT64"}}}}])"),
               ApiError);
}

}  // namespace
}  // namespace bigquery_emulator_duckdb
