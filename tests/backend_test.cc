#include "src/backend.h"

#include <string>

#include "gtest/gtest.h"
#include "nlohmann/json.hpp"

namespace bigquery_emulator_duckdb {
namespace {

using nlohmann::json;

TEST(BackendTest, ExecutesSelectOne) { EXPECT_EQ(ExecuteScalarString("SELECT 1"), "1"); }

TEST(BackendTest, ReturnsSchemaAndRows) {
  Backend backend;
  const QueryResult result = backend.Execute("SELECT 1 AS a, 'x' AS b, NULL AS c");
  ASSERT_TRUE(result.has_rows);
  EXPECT_EQ(result.SchemaToJson(), json::parse(R"({"fields": [
              {"name": "a", "type": "INTEGER", "mode": "NULLABLE"},
              {"name": "b", "type": "STRING", "mode": "NULLABLE"},
              {"name": "c", "type": "INTEGER", "mode": "NULLABLE"}]})"));
  ASSERT_EQ(result.rows.size(), 1);
  EXPECT_EQ(result.rows[0], json::parse(R"({"f": [{"v": "1"}, {"v": "x"}, {"v": null}]})"));
}

TEST(BackendTest, EncodesScalarTypes) {
  Backend backend;
  const QueryResult result = backend.Execute(R"(
      SELECT true AS b, 1.5::DOUBLE AS f, DATE '2020-01-02' AS d, TIME '12:34:56' AS t,
             TIMESTAMP '2020-01-02 03:04:05.5' AS dt,
             TIMESTAMPTZ '2020-01-02 03:04:05.25+00' AS ts,
             'aGk='::BLOB AS bytes, 1.25::DECIMAL(10, 2) AS n)");
  json fields = json::array();
  for (const FieldSchema& field : result.schema) {
    fields.push_back(field.type);
  }
  EXPECT_EQ(fields, json::parse(R"(["BOOLEAN", "FLOAT", "DATE", "TIME", "DATETIME",
                                     "TIMESTAMP", "BYTES", "NUMERIC"])"));
  EXPECT_EQ(result.rows[0]["f"],
            json::parse(R"([{"v": "true"}, {"v": "1.5"}, {"v": "2020-01-02"}, {"v": "12:34:56"},
                           {"v": "2020-01-02T03:04:05.5"}, {"v": "1577934245.25"},
                           {"v": "YUdrPQ=="}, {"v": "1.25"}])"));
}

TEST(BackendTest, EncodesArraysAndStructs) {
  Backend backend;
  const QueryResult result =
      backend.Execute("SELECT [1, 2] AS arr, {'x': 1, 'y': ['a']} AS s, NULL::INTEGER[] AS e");
  EXPECT_EQ(result.SchemaToJson(), json::parse(R"({"fields": [
              {"name": "arr", "type": "INTEGER", "mode": "REPEATED"},
              {"name": "s", "type": "RECORD", "mode": "NULLABLE", "fields": [
                {"name": "x", "type": "INTEGER", "mode": "NULLABLE"},
                {"name": "y", "type": "STRING", "mode": "REPEATED"}]},
              {"name": "e", "type": "INTEGER", "mode": "REPEATED"}]})"));
  EXPECT_EQ(result.rows[0]["f"], json::parse(R"([{"v": [{"v": "1"}, {"v": "2"}]},
                           {"v": {"f": [{"v": "1"}, {"v": [{"v": "a"}]}]}},
                           {"v": []}])"));
}

TEST(BackendTest, ReportsStatementsWithoutRows) {
  Backend backend;
  EXPECT_FALSE(backend.Execute("CREATE TABLE t (id INTEGER)").has_rows);
  const QueryResult insert = backend.Execute("INSERT INTO t VALUES (1), (2)");
  EXPECT_FALSE(insert.has_rows);
  EXPECT_EQ(insert.affected_rows, 2);
  EXPECT_EQ(backend.Execute("SELECT count(*) FROM t").rows[0]["f"][0]["v"], "2");
}

TEST(BackendTest, ThrowsOnError) {
  Backend backend;
  EXPECT_THROW(backend.Execute("SELECT * FROM missing"), BackendError);
}

}  // namespace
}  // namespace bigquery_emulator_duckdb
