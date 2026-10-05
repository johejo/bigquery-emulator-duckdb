#include "src/emulator.h"

#include <algorithm>
#include <filesystem>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "nlohmann/json.hpp"
#include "src/api_error.h"
#include "src/backend.h"
#include "src/field_schema.h"
#include "src/query_parameters.h"

namespace bigquery_emulator_duckdb {
namespace {

// Keep cases here that exercise the Emulator boundary beyond the client e2e scenarios.
class EmulatorTest : public ::testing::Test {
 protected:
  std::shared_ptr<const Job> Run(const std::string& sql) {
    QueryRequest request;
    request.project_id = "test";
    request.query = sql;
    return emulator_.RunQuery(request);
  }

  // The job's error message, or an empty string when the job succeeded.
  static std::string ErrorMessage(const Job& job) {
    return job.error.has_value() ? job.error->what() : "";
  }

  // The HTTP status the job's error would be reported with, or 0 when the query succeeded.
  int ErrorStatus(const std::string& sql) {
    const std::shared_ptr<const Job> job = Run(sql);
    return job->error.has_value() ? job->error->http_status() : 0;
  }

  // Runs `sql` and returns the only cell of its only row, or nullopt when that cell is NULL.
  std::optional<std::string> Scalar(const std::string& sql) {
    const std::shared_ptr<const Job> job = Run(sql);
    if (job->error.has_value()) {
      ADD_FAILURE() << sql << "\n  " << job->error->what();
      return std::nullopt;
    }
    if (!job->result.has_value()) {
      ADD_FAILURE() << sql << "\n  the job has neither a result nor an error";
      return std::nullopt;
    }
    const QueryResult& result = *job->result;
    EXPECT_EQ(result.schema.size(), 1) << sql;
    EXPECT_EQ(result.rows.size(), 1) << sql;
    if (result.schema.size() != 1 || result.rows.size() != 1) {
      return std::nullopt;
    }
    const nlohmann::json& value = result.rows[0]["f"][0]["v"];
    if (value.is_null()) {
      return std::nullopt;
    }
    return value.get<std::string>();
  }

  Emulator emulator_{"", {{.project_id = "test"}}};
};

TEST_F(EmulatorTest, RunsRenamedAggregates) {
  EXPECT_EQ(Scalar("SELECT LOGICAL_AND(a) FROM (SELECT true AS a UNION ALL SELECT false)"),
            "false");
  EXPECT_EQ(Scalar("SELECT LOGICAL_OR(a) FROM (SELECT true AS a UNION ALL SELECT false)"), "true");
  EXPECT_EQ(Scalar("SELECT COUNTIF(a > 1) FROM (SELECT 1 AS a UNION ALL SELECT 2)"), "1");
}

TEST_F(EmulatorTest, RunsResolvedByteLengthOverloads) {
  EXPECT_EQ(Scalar("SELECT BYTE_LENGTH('あ')"), "3");
  EXPECT_EQ(Scalar("SELECT BYTE_LENGTH(b'abc')"), "3");
  EXPECT_EQ(Scalar("SELECT BYTE_LENGTH(b'\\x00\\xff')"), "2");
  EXPECT_EQ(Scalar("SELECT BYTE_LENGTH('')"), "0");
  EXPECT_EQ(Scalar("SELECT BYTE_LENGTH(CAST(NULL AS STRING))"), std::nullopt);
  EXPECT_EQ(Scalar("SELECT BYTE_LENGTH(CAST(NULL AS BYTES))"), std::nullopt);
}

TEST_F(EmulatorTest, RunsAndPreparesResolvedParameters) {
  QueryRequest request;
  request.project_id = "test";
  request.parameters = QueryParameters::Parse(nlohmann::json::parse(R"([
    {"name":"s","parameterType":{"type":"STRING"},"parameterValue":{"value":"あ"}},
    {"name":"b","parameterType":{"type":"BYTES"},"parameterValue":{"value":"AP8="}}
  ])"));
  request.query = "SELECT BYTE_LENGTH(@s), BYTE_LENGTH(@b) AS bytes, SAFE_CAST(@s AS INT64)";
  for (const bool dry_run : {false, true}) {
    request.dry_run = dry_run;
    const auto job = emulator_.RunQuery(request);
    if (!job->result.has_value()) {
      FAIL() << ErrorMessage(*job);
    }
    const auto& result = job->result.value();
    ASSERT_EQ(result.schema.size(), 3);
    EXPECT_EQ(result.schema[0].name, "f0_");
    EXPECT_EQ(result.schema[1].name, "bytes");
    EXPECT_EQ(result.schema[2].name, "f1_");
    for (const auto& field : result.schema) {
      EXPECT_EQ(field.type, FieldType::kInteger);
    }
    if (!dry_run) {
      ASSERT_EQ(result.rows.size(), 1);
      EXPECT_EQ(result.rows[0]["f"][0]["v"], "3");
      EXPECT_EQ(result.rows[0]["f"][1]["v"], "2");
      EXPECT_TRUE(result.rows[0]["f"][2]["v"].is_null());
    }
  }
  request.dry_run = false;
  request.query = "SELECT CAST(@s AS INT64)";
  EXPECT_TRUE(emulator_.RunQuery(request)->error.has_value());
}

TEST_F(EmulatorTest, RunsCallsWithTheSafePrefix) {
  EXPECT_EQ(Scalar("SELECT SAFE.REGEXP_CONTAINS('abc', 'b')"), "true");
}

// The result schema follows BigQuery's typing and naming rather than DuckDB's.
TEST_F(EmulatorTest, ReportsTheResolvedResultSchema) {
  const std::shared_ptr<const Job> job =
      Run("SELECT SUM(x) AS s, ANY_VALUE('a'), 2.5, CURRENT_DATE() FROM (SELECT 1 AS x UNION ALL "
          "SELECT 2)");
  if (!job->result.has_value()) {
    FAIL() << ErrorMessage(*job);
  }
  const std::vector<FieldSchema>& schema = job->result->schema;
  ASSERT_EQ(schema.size(), 4);
  // DuckDB sums integers into a HUGEINT, which alone would be reported as BIGNUMERIC.
  EXPECT_EQ(schema[0].name, "s");
  EXPECT_EQ(schema[0].type, FieldType::kInteger);
  EXPECT_EQ(schema[1].name, "f0_");
  EXPECT_EQ(schema[1].type, FieldType::kString);
  EXPECT_EQ(schema[2].name, "f1_");
  EXPECT_EQ(schema[2].type, FieldType::kFloat);
  EXPECT_EQ(schema[3].name, "f2_");
  EXPECT_EQ(schema[3].type, FieldType::kDate);
}

// A value table of structs comes back as the structs' fields; any other value is one column.
TEST_F(EmulatorTest, ReturnsValueTablesAsColumns) {
  const std::shared_ptr<const Job> job = Run("SELECT AS STRUCT 1 AS a, 'x' AS b");
  if (!job->result.has_value()) {
    FAIL() << ErrorMessage(*job);
  }
  const std::vector<FieldSchema>& schema = job->result->schema;
  ASSERT_EQ(schema.size(), 2);
  EXPECT_EQ(schema[0].name, "a");
  EXPECT_EQ(schema[0].type, FieldType::kInteger);
  EXPECT_EQ(schema[1].name, "b");
  EXPECT_EQ(schema[1].type, FieldType::kString);
  ASSERT_EQ(job->result->rows.size(), 1);
  EXPECT_EQ(job->result->rows[0]["f"][1]["v"], "x");
  EXPECT_EQ(Scalar("SELECT AS VALUE 7"), "7");
}

TEST_F(EmulatorTest, AliasesUnnestedElements) {
  EXPECT_EQ(Scalar("SELECT SUM(x) FROM UNNEST([1, 2]) AS x"), "3");
}

TEST_F(EmulatorTest, AnalyzesQueriesAgainstTheTablesInDuckDb) {
  emulator_.CreateDataset({"test", "ds"});
  emulator_.CreateTable({"test", "ds", "t"}, {{.name = "a", .type = FieldType::kInteger},
                                              {.name = "b", .type = FieldType::kString}});
  QueryRequest request;
  request.project_id = "test";
  request.default_dataset = DatasetReference{"test", "ds"};
  request.query = "INSERT INTO t (a, b) VALUES (1, 'x'), (2, 'y')";
  ASSERT_FALSE(emulator_.RunQuery(request)->error.has_value());
  request.query = "SELECT SUM(a) FROM t";
  const std::shared_ptr<const Job> job = emulator_.RunQuery(request);
  if (!job->result.has_value()) {
    FAIL() << ErrorMessage(*job);
  }
  EXPECT_EQ(job->result->schema.at(0).name, "f0_");
  EXPECT_EQ(job->result->schema.at(0).type, FieldType::kInteger);
  EXPECT_EQ(job->result->rows.at(0)["f"][0]["v"], "3");

  request.query = "SELECT nope FROM t";
  const std::shared_ptr<const Job> failed = emulator_.RunQuery(request);
  ASSERT_TRUE(failed->error.has_value());
  EXPECT_NE(ErrorMessage(*failed).find("Unrecognized name: nope"), std::string::npos)
      << ErrorMessage(*failed);
}

TEST_F(EmulatorTest, TypesQueryParameters) {
  QueryRequest request;
  request.project_id = "test";
  request.parameters = QueryParameters::Parse(nlohmann::json::parse(R"([
      {"name": "n", "parameterType": {"type": "INT64"}, "parameterValue": {"value": "2"}},
      {"name": "s", "parameterType": {"type": "STRING"}, "parameterValue": {"value": "2"}}])"));
  request.query = "SELECT @n * 2 AS x";
  const std::shared_ptr<const Job> job = emulator_.RunQuery(request);
  if (!job->result.has_value()) {
    FAIL() << ErrorMessage(*job);
  }
  EXPECT_EQ(job->result->schema.at(0).type, FieldType::kInteger);
  EXPECT_EQ(job->result->rows.at(0)["f"][0]["v"], "4");

  // BigQuery does not coerce a STRING to a number, however DuckDB would.
  request.query = "SELECT @s * 2";
  EXPECT_TRUE(emulator_.RunQuery(request)->error.has_value());
  request.query = "SELECT @missing";
  EXPECT_TRUE(emulator_.RunQuery(request)->error.has_value());
}

// A table the statement itself creates need not exist yet, and unqualified names in DDL go to
// the default dataset the way they do in queries.
TEST_F(EmulatorTest, RunsDdlAgainstTheDefaultDataset) {
  emulator_.CreateDataset({"test", "ddl"});
  EXPECT_EQ(ErrorStatus("CREATE TABLE ddl.t (a INT64)"), 0);
  QueryRequest request;
  request.project_id = "test";
  request.default_dataset = DatasetReference{"test", "ddl"};
  request.query = "CREATE TABLE u AS SELECT 'x' AS b";
  EXPECT_EQ(ErrorMessage(*emulator_.RunQuery(request)), "");
  request.query = "SELECT b FROM u";
  const std::shared_ptr<const Job> job = emulator_.RunQuery(request);
  if (!job->result.has_value()) {
    FAIL() << ErrorMessage(*job);
  }
  EXPECT_EQ(job->result->rows.at(0)["f"][0]["v"], "x");
  EXPECT_EQ(emulator_.ListTables({"test", "ddl"}), (std::vector<std::string>{"t", "u"}));
  request.query = "DROP TABLE u";
  ASSERT_FALSE(emulator_.RunQuery(request)->error.has_value());
  EXPECT_EQ(ErrorStatus("DROP TABLE ddl.t"), 0);
  EXPECT_EQ(ErrorStatus("CREATE SCHEMA made"), 0);
  EXPECT_EQ(emulator_.ListDatasets("test"), (std::vector<std::string>{"ddl", "made"}));
  EXPECT_EQ(ErrorStatus("DROP SCHEMA made"), 0);
}

TEST_F(EmulatorTest, WritesQueryResultsToADestinationTable) {
  emulator_.CreateDataset({"test", "ds"});
  const TableReference destination{"test", "ds", "dest"};
  const auto write = [&](const std::string& sql, WriteDisposition write_disposition,
                         CreateDisposition create_disposition =
                             CreateDisposition::kCreateIfNeeded) {
    QueryRequest request;
    request.project_id = "test";
    request.query = sql;
    request.destination_table = destination;
    request.write_disposition = write_disposition;
    request.create_disposition = create_disposition;
    return emulator_.RunQuery(request);
  };
  const auto values = [&] {
    std::vector<std::string> cells;
    const auto data = emulator_.ListTableData(destination, 0, 100);
    std::ranges::transform(data.rows, std::back_inserter(cells), [](const nlohmann::json& row) {
      return row["f"][0]["v"].get<std::string>() + "/" + row["f"][1]["v"].get<std::string>();
    });
    return cells;
  };

  const std::shared_ptr<const Job> never =
      write("SELECT 1 AS a", WriteDisposition::kWriteEmpty, CreateDisposition::kCreateNever);
  if (!never->error.has_value()) {
    FAIL() << "expected an error";
  }
  EXPECT_EQ(never->error->http_status(), 404);

  // The table takes the query's schema, BigQuery's names and types included.
  const std::shared_ptr<const Job> created =
      write("SELECT 1 AS a, SUM(x) FROM UNNEST([2]) AS x", WriteDisposition::kWriteEmpty);
  if (!created->result.has_value()) {
    FAIL() << ErrorMessage(*created);
  }
  EXPECT_EQ(created->result->rows.size(), 1);
  const TableInfo table = emulator_.GetTable(destination);
  ASSERT_EQ(table.schema.size(), 2);
  EXPECT_EQ(table.schema[1].name, "f0_");
  EXPECT_EQ(table.schema[1].type, FieldType::kInteger);
  EXPECT_EQ(values(), (std::vector<std::string>{"1/2"}));

  const std::shared_ptr<const Job> not_empty =
      write("SELECT 3 AS a, 4 AS f0_", WriteDisposition::kWriteEmpty);
  if (!not_empty->error.has_value()) {
    FAIL() << "expected an error";
  }
  EXPECT_EQ(not_empty->error->http_status(), 409);

  // Appending matches columns by name.
  ASSERT_FALSE(write("SELECT 4 AS f0_, 3 AS a", WriteDisposition::kWriteAppend)->error.has_value());
  EXPECT_EQ(values(), (std::vector<std::string>{"1/2", "3/4"}));

  // The query can read the table it replaces.
  ASSERT_FALSE(write("SELECT a * 10 AS a, f0_ FROM ds.dest", WriteDisposition::kWriteTruncateData)
                   ->error.has_value());
  EXPECT_EQ(values(), (std::vector<std::string>{"10/2", "30/4"}));

  ASSERT_FALSE(
      write("SELECT 'x' AS b, 5 AS c", WriteDisposition::kWriteTruncate)->error.has_value());
  const TableInfo replaced = emulator_.GetTable(destination);
  ASSERT_EQ(replaced.schema.size(), 2);
  EXPECT_EQ(replaced.schema[0].name, "b");
  EXPECT_EQ(replaced.schema[0].type, FieldType::kString);
  EXPECT_EQ(values(), (std::vector<std::string>{"x/5"}));

  // A failed write leaves the table as it was.
  const std::shared_ptr<const Job> mismatch =
      write("SELECT 1 AS nope", WriteDisposition::kWriteAppend);
  ASSERT_TRUE(mismatch->error.has_value());
  EXPECT_EQ(values(), (std::vector<std::string>{"x/5"}));

  EXPECT_TRUE(write("SELECT 1 AS a, 2 AS A", WriteDisposition::kWriteTruncate)->error.has_value());
  EXPECT_TRUE(
      write("CREATE TABLE ds.other (a INT64)", WriteDisposition::kWriteEmpty)->error.has_value());
}

TEST_F(EmulatorTest, ReportsUnsupportedConstructsAsInvalidQuery) {
  EXPECT_EQ(ErrorStatus("SELECT SESSION_USER()"), 400);
  EXPECT_NE(
      ErrorMessage(*Run("SELECT SESSION_USER()")).find("does not support function SESSION_USER"),
      std::string::npos);
}

// A query that fails is reported through the job rather than thrown, which is how BigQuery
// reports it too.
TEST_F(EmulatorTest, ReportsAFailedQueryAsAJobError) {
  EXPECT_EQ(ErrorStatus("SELECT * FROM missing_table"), 400);
  EXPECT_EQ(ErrorStatus("SELECT FROM WHERE"), 400);
  EXPECT_EQ(ErrorStatus("SELECT 1"), 0);
}

// That data survives a restart is covered by tests/e2e/restart; this checks the file layout.
TEST(EmulatorPersistenceTest, StoresEachProjectInItsOwnFile) {
  const std::filesystem::path data_dir =
      std::filesystem::path(::testing::TempDir()) / "emulator_persistence";
  std::filesystem::remove_all(data_dir);
  {
    Emulator emulator(data_dir.string(),
                      {{.project_id = "proj"}, {.project_id = "example.com:proj"}});
    emulator.CreateDataset({"proj", "ds"});
    // A domain-scoped id checks that ':' and '.' are encoded into a single file name.
    emulator.CreateDataset({"example.com:proj", "scoped"});
  }
  EXPECT_TRUE(std::filesystem::exists(data_dir / "proj.duckdb"));
  EXPECT_TRUE(std::filesystem::exists(data_dir / "example%2Ecom%3Aproj.duckdb"));
}

// A view written to the project file by other means has no GoogleSQL definition or schema.
TEST(EmulatorPersistenceTest, HandlesViewsWithoutMetadata) {
  const std::filesystem::path data_dir =
      std::filesystem::path(::testing::TempDir()) / "emulator_foreign_view";
  std::filesystem::remove_all(data_dir);
  std::filesystem::create_directories(data_dir);
  {
    Backend backend;
    backend.Execute("ATTACH '" + (data_dir / "proj.duckdb").string() + "' AS proj");
    backend.Execute("CREATE SCHEMA proj.ds");
    backend.Execute("CREATE VIEW proj.ds.v AS SELECT 1 AS x");
  }
  Emulator emulator(data_dir.string(), {{.project_id = "proj"}});
  const std::vector<TableListEntry> entries = emulator.ListTableEntries({"proj", "ds"});
  ASSERT_EQ(entries.size(), 1);
  EXPECT_EQ(entries[0].table_id, "v");
  EXPECT_EQ(entries[0].type, TableType::kView);
  try {
    emulator.GetTable({"proj", "ds", "v"});
    ADD_FAILURE() << "GetTable succeeded";
  } catch (const ApiError& error) {
    EXPECT_EQ(error.http_status(), 400);
    EXPECT_NE(std::string(error.what()).find("was not created by the emulator"), std::string::npos)
        << error.what();
  }

  QueryRequest request;
  request.project_id = "proj";
  request.query =
      "SELECT table_type, ddl IS NULL FROM ds.INFORMATION_SCHEMA.TABLES WHERE table_name = 'v'";
  const std::shared_ptr<const Job> job = emulator.RunQuery(request);
  if (job->error.has_value()) {
    FAIL() << job->error->what();
  }
  if (!job->result.has_value()) {
    FAIL() << "the job has neither a result nor an error";
  }
  const QueryResult& result = *job->result;
  ASSERT_EQ(result.rows.size(), 1);
  EXPECT_EQ(result.rows[0]["f"][0]["v"], "VIEW");
  EXPECT_EQ(result.rows[0]["f"][1]["v"], "true");

  emulator.DeleteTable({"proj", "ds", "v"});
  EXPECT_TRUE(emulator.ListTables({"proj", "ds"}).empty());
}

}  // namespace
}  // namespace bigquery_emulator_duckdb
