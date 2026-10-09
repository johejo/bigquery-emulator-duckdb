#include "src/field_schema.h"

#include "gtest/gtest.h"
#include "nlohmann/json.hpp"
#include "src/api_error.h"

namespace bigquery_emulator_duckdb {
namespace {

using nlohmann::json;

TEST(FieldSchemaTest, AcceptsStandardSqlTypeNamesInAnyCase) {
  const FieldSchema field = FieldSchemaFromJson(json::parse(R"({
      "name": "s", "type": "struct", "mode": "repeated",
      "fields": [{"name": "a", "type": "INT64"}, {"name": "b", "type": "Bool"}]})"));
  EXPECT_EQ(field.type, FieldType::kRecord);
  EXPECT_EQ(field.mode, FieldMode::kRepeated);
  EXPECT_EQ(field.fields.at(0).type, FieldType::kInteger);
  EXPECT_EQ(field.fields.at(0).mode, FieldMode::kNullable);
  EXPECT_EQ(field.fields.at(1).type, FieldType::kBoolean);
  EXPECT_EQ(field.ToJson()["type"], "RECORD");
}

TEST(FieldSchemaTest, DefaultsToANullableString) {
  const FieldSchema field = FieldSchemaFromJson(json::parse(R"({"name": "c"})"));
  EXPECT_EQ(field.ToJson(), json::parse(R"({"name": "c", "type": "STRING", "mode": "NULLABLE"})"));
}

TEST(FieldSchemaTest, RoundTripsColumnOptions) {
  const json value = json::parse(R"({
      "name": "n", "type": "NUMERIC", "mode": "REQUIRED", "description": "amount",
      "precision": "10", "scale": "2", "defaultValueExpression": "0",
      "policyTags": {"names": ["projects/p/locations/us/taxonomies/1/policyTags/2"]}})");
  EXPECT_EQ(FieldSchemaFromJson(value).ToJson(), value);
  // BigQuery encodes int64 as strings; numbers are accepted and written back as strings.
  const FieldSchema field = FieldSchemaFromJson(json::parse(R"({"name": "s", "maxLength": 5})"));
  EXPECT_EQ(field.max_length, 5);
  EXPECT_EQ(field.ToJson()["maxLength"], "5");
}

TEST(FieldSchemaTest, RoundTripsRangeElementType) {
  const json value = json::parse(R"({
      "name": "r", "type": "RANGE", "mode": "NULLABLE", "rangeElementType": {"type": "DATE"}})");
  const FieldSchema field = FieldSchemaFromJson(value);
  EXPECT_EQ(field.range_element_type, FieldType::kDate);
  EXPECT_EQ(field.ToJson(), value);
  // Only a RANGE has an element type.
  EXPECT_FALSE(FieldSchemaFromJson(json::parse(R"({
      "name": "d", "type": "DATE", "rangeElementType": {"type": "DATE"}})"))
                   .range_element_type.has_value());
}

TEST(FieldSchemaTest, RejectsWhatBigQueryRejects) {
  EXPECT_THROW(FieldSchemaFromJson(json::parse(R"({"name": "r", "type": "RANGE"})")), ApiError);
  EXPECT_THROW(FieldSchemaFromJson(json::parse(
                   R"({"name": "r", "type": "RANGE", "rangeElementType": {"type": "TIME"}})")),
               ApiError);
  EXPECT_THROW(FieldSchemaFromJson(json::parse(R"({"type": "STRING"})")), ApiError);
  EXPECT_THROW(FieldSchemaFromJson(json::parse(R"({"name": "c", "type": "NOPE"})")), ApiError);
  EXPECT_THROW(FieldSchemaFromJson(json::parse(R"({"name": "c", "mode": "OPTIONAL"})")), ApiError);
  EXPECT_THROW(FieldSchemaFromJson(json::parse(R"({"name": "c", "maxLength": "x"})")), ApiError);
}

}  // namespace
}  // namespace bigquery_emulator_duckdb
