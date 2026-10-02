#include "src/column_metadata.h"

#include <vector>

#include "gtest/gtest.h"
#include "nlohmann/json.hpp"
#include "src/field_schema.h"

namespace bigquery_emulator_duckdb {
namespace {

using nlohmann::json;

TEST(ColumnMetadataTest, KeepsTheDerivedFieldWithoutAFieldComment) {
  const std::vector<FieldSchema> derived = {
      {.name = "a"}, {.name = "b"}, {.name = "c"}, {.name = "d"}, {.name = "e"}};
  const std::vector<FieldSchema> schema = ApplyColumnComments(
      derived, {json(R"({"name": "a", "type": "JSON", "mode": "REQUIRED"})"),
                // Comments the emulator did not write, or that describe another column.
                json("written by someone else"), json(R"({"name": "other", "type": "JSON"})"),
                json(R"({"name": "d", "type": "NOPE"})"), json(nullptr)});
  EXPECT_EQ(schema[0].type, FieldType::kJson);
  EXPECT_EQ(schema[0].mode, FieldMode::kRequired);
  for (size_t i = 1; i < schema.size(); ++i) {
    EXPECT_EQ(schema[i].ToJson(), derived[i].ToJson()) << i;
  }
}

}  // namespace
}  // namespace bigquery_emulator_duckdb
