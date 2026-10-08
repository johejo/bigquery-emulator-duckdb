#include "src/backend.h"

#include <cstddef>
#include <future>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "nlohmann/json.hpp"
#include "src/backend_error.h"
#include "src/field_schema.h"

namespace bigquery_emulator_duckdb {
namespace {

using nlohmann::json;

TEST(BackendTest, ExecutesSelectOne) { EXPECT_EQ(ExecuteScalarString("SELECT 1"), "1"); }

TEST(BackendTest, SessionIdentitiesAreIsolatedAcrossConcurrentDatabases) {
  Backend first("first@example.com");
  Backend second("second@example.com");
  const auto read = [](Backend& backend) {
    auto session = backend.NewSession();
    session->Transaction("BEGIN TRANSACTION");
    const QueryResult result = session->Execute("SELECT bq_session_user()");
    session->Transaction("COMMIT");
    return result.rows.at(0).at("f").at(0).at("v");
  };
  auto a = std::async(std::launch::async, [&] { return read(first); });
  auto b = std::async(std::launch::async, [&] { return read(second); });
  EXPECT_EQ(a.get(), "first@example.com");
  EXPECT_EQ(b.get(), "second@example.com");
}

TEST(BackendTest, RollsBackViewCreationWhenMetadataFails) {
  Backend backend;
  EXPECT_THROW(backend.ExecuteDdl("CREATE VIEW v AS SELECT 1 AS x",
                                  {"COMMENT ON VIEW missing IS 'metadata'"}, ""),
               BackendError);
  EXPECT_THROW(backend.Prepare("SELECT * FROM v"), BackendError);
  backend.ExecuteDdl("CREATE VIEW v AS SELECT 2 AS x", {"COMMENT ON VIEW v IS 'original'"}, "");
  EXPECT_THROW(backend.ExecuteDdl("CREATE OR REPLACE VIEW v AS SELECT 3 AS y",
                                  {"COMMENT ON VIEW missing IS 'metadata'"}, ""),
               BackendError);
  const QueryResult metadata =
      backend.Execute("SELECT comment FROM duckdb_views() WHERE view_name = 'v'");
  ASSERT_EQ(metadata.rows.size(), 1);
  EXPECT_EQ(metadata.rows.at(0)["f"][0]["v"], "original");
  EXPECT_EQ(backend.Prepare("SELECT * FROM v").schema.at(0).name, "x");
}

TEST(BackendTest, ReturnsSchemaAndRows) {
  Backend backend;
  const QueryResult result = backend.Execute("SELECT 1 AS a, 'x' AS b, NULL AS c");
  ASSERT_TRUE(result.has_rows);
  EXPECT_EQ(result.SchemaToJson(), json::parse(R"({"fields": [
              {"name": "a", "type": "INTEGER", "mode": "NULLABLE"},
              {"name": "b", "type": "STRING", "mode": "NULLABLE"},
              {"name": "c", "type": "INTEGER", "mode": "NULLABLE"}]})"));
  ASSERT_EQ(result.rows.size(), 1);
  EXPECT_EQ(result.rows.at(0), json::parse(R"({"f": [{"v": "1"}, {"v": "x"}, {"v": null}]})"));
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
    fields.push_back(FieldTypeName(field.type));
  }
  EXPECT_EQ(fields, json::parse(R"(["BOOLEAN", "FLOAT", "DATE", "TIME", "DATETIME",
                                     "TIMESTAMP", "BYTES", "NUMERIC"])"));
  EXPECT_EQ(result.rows.at(0)["f"],
            json::parse(R"([{"v": "true"}, {"v": "1.5"}, {"v": "2020-01-02"}, {"v": "12:34:56"},
                           {"v": "2020-01-02T03:04:05.5"}, {"v": "1577934245250000"},
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
  EXPECT_EQ(result.rows.at(0)["f"], json::parse(R"([{"v": [{"v": "1"}, {"v": "2"}]},
                           {"v": {"f": [{"v": "1"}, {"v": [{"v": "a"}]}]}},
                           {"v": []}])"));
}

// The emulator keeps NULL arrays apart from empty ones where it holds the values itself, such as
// in the variables of a multi-statement query.
TEST(BackendTest, KeepsNullArraysWhenAsked) {
  Backend backend;
  const QueryResult result = backend.Execute(
      "SELECT NULL::INTEGER[] AS e, {'y': NULL::VARCHAR[]} AS s, []::INTEGER[] AS a", {}, true);
  EXPECT_EQ(result.rows.at(0)["f"], json::parse(R"([{"v": null}, {"v": {"f": [{"v": null}]}},
                           {"v": []}])"));
}

TEST(BackendTest, ReportsStatementsWithoutRows) {
  Backend backend;
  EXPECT_FALSE(backend.Execute("CREATE TABLE t (id INTEGER)").has_rows);
  const QueryResult insert = backend.Execute("INSERT INTO t VALUES (1), (2)");
  EXPECT_FALSE(insert.has_rows);
  EXPECT_EQ(insert.affected_rows, 2);
  EXPECT_EQ(backend.Execute("SELECT count(*) FROM t").rows.at(0)["f"][0]["v"], "2");
}

TEST(BackendTest, ConvertsTimestampsToSeconds) {
  Backend backend;
  const QueryResult result = backend.Execute(R"(
      SELECT TIMESTAMPTZ '2020-01-02 03:04:05.25+00' AS ts,
             [TIMESTAMPTZ '1970-01-01 00:00:01+00'] AS tss,
             {'inner': TIMESTAMPTZ '1969-12-31 23:59:59+00'} AS s,
             NULL::TIMESTAMPTZ AS n, 'x' AS other)");
  EXPECT_TRUE(HasTimestampField(result.schema));
  EXPECT_EQ(TimestampsAsSeconds(result.schema, result.rows.at(0))["f"],
            json::parse(R"([{"v": "1577934245.25"}, {"v": [{"v": "1"}]},
                           {"v": {"f": [{"v": "-1"}]}}, {"v": null}, {"v": "x"}])"));
}

TEST(BackendTest, LeavesResultsWithoutTimestampsAlone) {
  Backend backend;
  const QueryResult result = backend.Execute("SELECT 1 AS a, TIMESTAMP '2020-01-02' AS datetime");
  EXPECT_FALSE(HasTimestampField(result.schema));
}

TEST(BackendTest, PreparesWithoutExecuting) {
  Backend backend;
  backend.Execute("CREATE TABLE t (id INTEGER)");
  const QueryResult prepared = backend.Prepare("SELECT id, 'x' AS s FROM t");
  EXPECT_TRUE(prepared.has_rows);
  EXPECT_TRUE(prepared.rows.empty());
  EXPECT_EQ(prepared.SchemaToJson(), json::parse(R"({"fields": [
              {"name": "id", "type": "INTEGER", "mode": "NULLABLE"},
              {"name": "s", "type": "STRING", "mode": "NULLABLE"}]})"));

  // A statement that produces no result set has no schema, and preparing it leaves the table
  // untouched.
  EXPECT_FALSE(backend.Prepare("INSERT INTO t VALUES (1)").has_rows);
  EXPECT_EQ(backend.Execute("SELECT count(*) FROM t").rows.at(0)["f"][0]["v"], "0");
  EXPECT_THROW(backend.Prepare("SELECT * FROM missing"), BackendError);
}

TEST(BackendTest, EncodesFixedArraysAndNullStructs) {
  Backend backend;
  const std::string sql = R"(
      SELECT [{'x': 1}, {'x': NULL}]::STRUCT(x INTEGER)[2] AS a,
             NULL::STRUCT(x INTEGER) AS s, NULL::INTEGER[2] AS n)";
  const QueryResult result = backend.Execute(sql);
  EXPECT_EQ(result.schema.at(0).type, FieldType::kRecord);
  EXPECT_EQ(result.schema.at(0).mode, FieldMode::kRepeated);
  ASSERT_EQ(result.schema.at(0).fields.size(), 1);
  EXPECT_EQ(result.schema.at(0).fields.at(0).type, FieldType::kInteger);
  EXPECT_EQ(result.rows.at(0)["f"], json::parse(R"([
      {"v": [{"v": {"f": [{"v": "1"}]}}, {"v": {"f": [{"v": null}]}}]},
      {"v": null}, {"v": []}])"));
  EXPECT_EQ(backend.Prepare(sql).SchemaToJson(), result.SchemaToJson());
}

TEST(BackendTest, PreservesBinaryStringsAndDecimalPrecision) {
  Backend backend;
  const QueryResult result = backend.Execute(R"(
      SELECT chr(0) || 'abc' AS s, from_hex('00ff10') AS b,
             ''::BLOB AS empty, 'a'::BLOB AS one, 'ab'::BLOB AS two,
             -1.23::DECIMAL(4,2) AS d16, -12345.67::DECIMAL(9,2) AS d32,
             -1234567890.123456::DECIMAL(18,6) AS d64,
             -12345678901234567890.1234567890::DECIMAL(38,10) AS d128,
             '00112233-4455-6677-8899-aabbccddeeff'::UUID AS uuid)");
  EXPECT_EQ(result.rows.at(0)["f"][0]["v"], std::string("\0abc", 4));
  EXPECT_EQ(result.rows.at(0)["f"][1]["v"], "AP8Q");
  EXPECT_EQ(result.rows.at(0)["f"][2]["v"], "");
  EXPECT_EQ(result.rows.at(0)["f"][3]["v"], "YQ==");
  EXPECT_EQ(result.rows.at(0)["f"][4]["v"], "YWI=");
  EXPECT_EQ(result.rows.at(0)["f"][5]["v"], "-1.23");
  EXPECT_EQ(result.rows.at(0)["f"][6]["v"], "-12345.67");
  EXPECT_EQ(result.rows.at(0)["f"][7]["v"], "-1234567890.123456");
  EXPECT_EQ(result.rows.at(0)["f"][8]["v"], "-12345678901234567890.123456789");
  EXPECT_EQ(result.rows.at(0)["f"][9]["v"], "00112233-4455-6677-8899-aabbccddeeff");
  EXPECT_EQ(result.schema.at(8).type, FieldType::kNumeric);
}

// A BIGNUM is the integer number of units of 10^-38 of a BIGNUMERIC, over its whole range. Its
// bytes are decoded by hand, so the boundaries of a byte and negative values matter.
TEST(BackendTest, ReadsBigNumAsBigNumeric) {
  Backend backend;
  const QueryResult result = backend.Execute(R"(
      SELECT x::BIGNUM AS n FROM (VALUES ('0'), ('1'), ('-1'), ('255'), ('-256'),
          ('100000000000000000000000000000000000000'),
          ('57896044618658097711785492504343953926634992332820282019728792003956564819967'),
          ('-57896044618658097711785492504343953926634992332820282019728792003956564819968'))
      AS t(x))");
  EXPECT_EQ(result.schema.at(0).type, FieldType::kBigNumeric);
  ASSERT_EQ(result.rows.size(), 8);
  const std::vector<std::string> expected = {
      "0",
      "0.00000000000000000000000000000000000001",
      "-0.00000000000000000000000000000000000001",
      "0.00000000000000000000000000000000000255",
      "-0.00000000000000000000000000000000000256",
      "1",
      "578960446186580977117854925043439539266.34992332820282019728792003956564819967",
      "-578960446186580977117854925043439539266.34992332820282019728792003956564819968",
  };
  for (size_t i = 0; i < expected.size(); ++i) {
    EXPECT_EQ(result.rows.at(i)["f"][0]["v"], expected.at(i)) << i;
  }
  EXPECT_THROW(backend.Execute("SELECT "
                               "'578960446186580977117854925043439539266349923328202820197287920039"
                               "56564819968'::BIGNUM"),
               BackendError);
}

TEST(BackendTest, FormatsOtherDuckDBScalarTypes) {
  Backend backend;
  const QueryResult result = backend.Execute(R"(
      SELECT 'happy'::ENUM('sad', 'happy') AS e, '101'::BIT AS b,
             MAP {'x': [1, NULL]} AS m, union_value(s := 'hello') AS u,
             '18446744073709551615'::UBIGINT AS ui,
             '-170141183460469231731687303715884105728'::HUGEINT AS hi,
             TIMESTAMP_NS '2020-01-02 03:04:05.123456789' AS ns,
             TIMESTAMP_NS 'infinity' AS inf)");
  EXPECT_EQ(result.rows.at(0)["f"], json::parse(R"([
      {"v": "happy"}, {"v": "101"}, {"v": "{x=[1, NULL]}"}, {"v": "hello"},
      {"v": "18446744073709551615"}, {"v": "-170141183460469231731687303715884105728"},
      {"v": "2020-01-02T03:04:05.123456"}, {"v": "infinity"}])"));
}

TEST(BackendTest, ReadsMultipleChunks) {
  Backend backend;
  const QueryResult result = backend.Execute("SELECT i, [i, NULL] AS a FROM range(5000) t(i)");
  ASSERT_EQ(result.rows.size(), 5000);
  for (size_t i = 0; i < result.rows.size(); ++i) {
    EXPECT_EQ(result.rows.at(i)["f"][0]["v"], std::to_string(i));
    EXPECT_EQ(result.rows.at(i)["f"][1]["v"][0]["v"], std::to_string(i));
    EXPECT_TRUE(result.rows.at(i)["f"][1]["v"][1]["v"].is_null());
  }
}

TEST(BackendTest, RollsBackFailedTransactionsAndCanSkipInvalidRows) {
  Backend backend;
  backend.Execute("CREATE TABLE t (id INTEGER PRIMARY KEY)");
  EXPECT_THROW(backend.ExecuteAll({"BEGIN", "INSERT INTO t VALUES (1)", "SELECT * FROM missing"}),
               BackendError);
  EXPECT_EQ(backend.Execute("SELECT count(*) FROM t").rows.at(0)["f"][0]["v"], "0");
  const std::vector<std::string> statements = {
      "INSERT INTO t VALUES (1)",
      "INSERT INTO t VALUES (1)",
      "INSERT INTO t VALUES (2)",
  };
  auto errors = backend.InsertRows(statements, false);
  ASSERT_EQ(errors.size(), 1);
  EXPECT_EQ(errors.at(0).first, 1);
  EXPECT_FALSE(errors.at(0).second.empty());
  EXPECT_EQ(backend.Execute("SELECT count(*) FROM t").rows.at(0)["f"][0]["v"], "0");
  errors = backend.InsertRows(statements, true);
  ASSERT_EQ(errors.size(), 1);
  EXPECT_EQ(errors.at(0).first, 1);
  EXPECT_EQ(backend.Execute("UPDATE t SET id = id + 10").affected_rows, 2);
  EXPECT_EQ(backend.Execute("DELETE FROM t WHERE id = 999").affected_rows, 0);
  EXPECT_EQ(backend.Execute("DELETE FROM t").affected_rows, 2);
}

TEST(BackendTest, ScalarHelperHandlesNullEmptyAndErrors) {
  EXPECT_EQ(ExecuteScalarString("SELECT NULL"), "NULL");
  EXPECT_EQ(ExecuteScalarString("SELECT chr(0) || 'x'"), std::string("\0x", 2));
  EXPECT_THROW(ExecuteScalarString("SELECT 1 WHERE false"), BackendError);
  EXPECT_THROW(ExecuteScalarString("SELECT * FROM missing"), BackendError);
}

TEST(BackendTest, ThrowsOnError) {
  Backend backend;
  EXPECT_THROW(backend.Execute("SELECT * FROM missing"), BackendError);
}

}  // namespace
}  // namespace bigquery_emulator_duckdb
