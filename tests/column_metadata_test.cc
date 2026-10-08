#include "src/column_metadata.h"

#include <vector>

#include "gtest/gtest.h"
#include "nlohmann/json.hpp"
#include "src/api_error.h"
#include "src/field_schema.h"

namespace bigquery_emulator_duckdb {
namespace {

using nlohmann::json;

TEST(ColumnMetadataTest, KeepsTheDerivedFieldWithoutAFieldComment) {
  const std::vector<FieldSchema> derived = {
      {.name = "a"}, {.name = "b"}, {.name = "c"}, {.name = "d"}, {.name = "e"},
  };
  const std::vector<FieldSchema> schema = ApplyColumnComments(
      derived, {
                   json(R"({"name": "a", "type": "JSON", "mode": "REQUIRED"})"),
                   // Comments the emulator did not write, or that describe another column.
                   json("written by someone else"),
                   json(R"({"name": "other", "type": "JSON"})"),
                   json(R"({"name": "d", "type": "NOPE"})"),
                   json(nullptr),
               });
  EXPECT_EQ(schema.at(0).type, FieldType::kJson);
  EXPECT_EQ(schema.at(0).mode, FieldMode::kRequired);
  for (size_t i = 1; i < schema.size(); ++i) {
    EXPECT_EQ(schema.at(i).ToJson(), derived.at(i).ToJson()) << i;
  }
}

// An earlier emulator stored BIGNUMERIC as DECIMAL(38, 19), which reads back as NUMERIC and
// cannot hold every BIGNUMERIC.
TEST(ColumnMetadataTest, RejectsABigNumericStoredAsADecimal) {
  EXPECT_THROW(ApplyColumnComments({{.name = "n", .type = FieldType::kNumeric}},
                                   {json(R"({"name": "n", "type": "BIGNUMERIC"})")}),
               ApiError);
  EXPECT_THROW(
      ApplyColumnComments(
          {{.name = "s",
            .type = FieldType::kRecord,
            .fields = {{.name = "n", .type = FieldType::kNumeric}}}},
          {json(
              R"({"name": "s", "type": "RECORD", "fields": [{"name": "n", "type": "BIGNUMERIC"}]})")}),
      ApiError);
  EXPECT_EQ(ApplyColumnComments({{.name = "n", .type = FieldType::kBigNumeric}},
                                {json(R"({"name": "n", "type": "BIGNUMERIC"})")})
                .at(0)
                .type,
            FieldType::kBigNumeric);
}

}  // namespace
}  // namespace bigquery_emulator_duckdb
