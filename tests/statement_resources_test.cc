#include "src/statement_resources.h"

#include <barrier>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "googlesql/public/types/type_factory.h"
#include "gtest/gtest.h"
#include "src/analyzer.h"
#include "src/backend.h"
#include "src/backend_error.h"
#include "src/catalog.h"
#include "src/default_dataset.h"
#include "src/emulator_functions.h"
#include "src/field_schema.h"
#include "src/file_read.h"
#include "src/file_read_options.h"
#include "src/function_options.h"
#include "src/references.h"
#include "src/result_rows.h"
#include "src/storage_snapshot.h"
#include "src/table_source.h"
#include "src/temporary_files.h"
#include "src/timestamp_precision.h"
#include "src/translated_statement.h"
#include "src/translator.h"

namespace bigquery_emulator_duckdb {
namespace {

using Preparation = std::function<std::string(TemporaryFiles&)>;

// Inject staged sources to observe file lifetimes and force preparation failures.
class FileSource : public TableSource {
 public:
  explicit FileSource(Preparation prepare) : prepare_(std::move(prepare)) {}

  std::optional<std::vector<FieldSchema>> FindTable(const std::string& /*project*/,
                                                    const std::string& /*dataset*/,
                                                    const std::string& table) override {
    if (table != "t") {
      return std::nullopt;
    }
    return std::vector<FieldSchema>{{.name = "value", .type = FieldType::kInteger}};
  }

  std::optional<TableRead> ReadQuery(const TableReference& /*table*/) override { return prepare_; }

 private:
  Preparation prepare_;
};

std::string FileQuery(const std::string& path) {
  // Exercise the same reader as load jobs through a statement-owned source.
  return FileReadQuery(CsvReadOptions{.skip_leading_rows = 1}, {{.uri = path, .path = path}},
                       {{.name = "value", .type = FieldType::kInteger}});
}

TranslatedStatement TranslateRead(const std::string& query, Preparation prepare) {
  FileSource source(std::move(prepare));
  googlesql::TypeFactory types;
  BigQueryCatalog catalog(source, &types, "test", "d", TimestampPrecision::kMicroseconds);
  const auto analyzed =
      AnalyzeGoogleSql(query, catalog, types,
                       {
                           .default_project = "test",
                           .default_dataset = "d",
                           .timestamp_precision = TimestampPrecision::kMicroseconds,
                       });
  std::string unsupported;
  auto translation = TranslateStatement(
      EmulatorFunctionCatalog(BigQueryFunctionOptions()), analyzed.statement(), {},
      DefaultDataset{
          .project = "test",
          .dataset = "d",
          .source = &source,
          .resources = std::make_shared<StatementResources>(StatementResources::Mode::kExecute),
      },
      &unsupported);
  if (!translation) {
    throw std::runtime_error(unsupported);
  }
  return *std::move(translation);
}

ResultRows ExecuteRead(Backend& backend, const TranslatedStatement& statement) {
  auto result = backend.Execute(statement.sql);
  result.rows.KeepResources(statement.resources);
  return std::move(result.rows);
}

TEST(StatementResourcesTest, KeepsFilesWithCopiedRows) {
  Backend backend;
  std::string path;
  ResultRows rows;
  {
    const auto statement = TranslateRead("SELECT value FROM t", [&](TemporaryFiles& files) {
      path = files.Write("value\n7\n");
      return FileQuery(path);
    });
    // The analyzer, catalog and source are already destroyed before execution starts.
    rows = ExecuteRead(backend, statement);
  }
  auto copy = rows;
  rows = {};
  EXPECT_TRUE(std::filesystem::exists(path));
  ASSERT_EQ(copy.size(), 1);
  EXPECT_EQ(copy.Int64(0, 0), 7);
  copy = {};
  EXPECT_FALSE(std::filesystem::exists(path));
}

TEST(StatementResourcesTest, CleansUpOnPreparationTranslationAndExecutionErrors) {
  Backend backend;
  std::string path;
  EXPECT_THROW(TranslateRead("SELECT value FROM t",
                             [&](TemporaryFiles& files) -> std::string {
                               path = files.Write("value\n7\n");
                               throw std::runtime_error("injected preparation failure");
                             }),
               std::runtime_error);
  EXPECT_FALSE(std::filesystem::exists(path));

  path.clear();
  // The first input is prepared before translation rejects the later hinted scan.
  EXPECT_THROW(TranslateRead("SELECT value FROM t UNION ALL "
                             "SELECT value FROM t @{test_hint=1}",
                             [&](TemporaryFiles& files) {
                               path = files.Write("value\n7\n");
                               return FileQuery(path);
                             }),
               std::runtime_error);
  EXPECT_FALSE(path.empty());
  EXPECT_FALSE(std::filesystem::exists(path));

  EXPECT_THROW(
      {
        const auto statement = TranslateRead("SELECT value FROM t", [&](TemporaryFiles& files) {
          path = files.Write("value\nnot_an_integer\n");
          return FileQuery(path);
        });
        ExecuteRead(backend, statement);
      },
      BackendError);
  EXPECT_FALSE(std::filesystem::exists(path));
}

TEST(StatementResourcesTest, ConcurrentStatementsDoNotSharePreparation) {
  Backend backend;
  std::barrier staged(2);
  const auto run = [&] {
    std::string path;
    const auto statement = TranslateRead("SELECT value FROM t", [&](TemporaryFiles& files) {
      path = files.Write("value\n7\n");
      staged.arrive_and_wait();
      return FileQuery(path);
    });
    return std::pair(path, ExecuteRead(backend, statement));
  };
  auto first_job = std::async(std::launch::async, run);
  auto second_job = std::async(std::launch::async, run);
  auto first = first_job.get();
  auto second = second_job.get();
  EXPECT_NE(first.first, second.first);
  first.second = {};
  EXPECT_FALSE(std::filesystem::exists(first.first));
  EXPECT_TRUE(std::filesystem::exists(second.first));
  ASSERT_EQ(second.second.size(), 1);
  EXPECT_EQ(second.second.Int64(0, 0), 7);
  second.second = {};
  EXPECT_FALSE(std::filesystem::exists(second.first));
}

TEST(StatementResourcesTest, ScalarExpressionKeepsItsOwnExecutionScope) {
  Backend backend;
  std::string path;
  {
    FileSource source([&](TemporaryFiles& files) {
      path = files.Write("value\n7\n");
      return FileQuery(path);
    });
    googlesql::TypeFactory types;
    BigQueryCatalog catalog(source, &types, "test", "d", TimestampPrecision::kMicroseconds);
    const auto analyzed =
        AnalyzeScriptExpression("(SELECT value FROM t)", nullptr, nullptr, catalog, types,
                                {
                                    .default_project = "test",
                                    .default_dataset = "d",
                                    .timestamp_precision = TimestampPrecision::kMicroseconds,
                                });
    ASSERT_TRUE(analyzed.ok()) << analyzed.status();
    const auto resources = std::make_shared<StatementResources>(StatementResources::Mode::kExecute);
    std::string unsupported;
    const auto query = TranslateExpression(EmulatorFunctionCatalog(BigQueryFunctionOptions()),
                                           analyzed->expression(), {},
                                           DefaultDataset{
                                               .project = "test",
                                               .dataset = "d",
                                               .source = &source,
                                               .resources = resources,
                                           },
                                           &unsupported, nullptr);
    ASSERT_TRUE(query.has_value()) << unsupported;
    EXPECT_NO_THROW(backend.Execute(*query));
    EXPECT_TRUE(std::filesystem::exists(path));
  }
  EXPECT_FALSE(std::filesystem::exists(path));
}

TEST(StatementResourcesTest, StorageSnapshotRetainsFilesThroughConsumption) {
  Backend backend;
  std::string path;
  std::shared_ptr<const StorageSnapshot> snapshot;
  {
    const auto statement = TranslateRead("SELECT value FROM t", [&](TemporaryFiles& files) {
      path = files.Write("value\n7\n");
      return FileQuery(path);
    });
    ASSERT_TRUE(statement.result_schema.has_value());
    snapshot =
        backend.ExecuteStorageRead(statement.sql, *statement.result_schema, statement.resources);
  }
  EXPECT_TRUE(std::filesystem::exists(path));
  ASSERT_EQ(snapshot->row_count(), 1);
  EXPECT_FALSE(snapshot->ReadRows(0, 1, false).empty());
  snapshot.reset();
  EXPECT_FALSE(std::filesystem::exists(path));
}

}  // namespace
}  // namespace bigquery_emulator_duckdb
