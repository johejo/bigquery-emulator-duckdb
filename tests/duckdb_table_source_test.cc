#include "src/duckdb_table_source.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "gtest/gtest.h"
#include "src/api_error.h"
#include "src/backend.h"
#include "src/duckdb_sql.h"
#include "src/external_table_provider.h"
#include "src/field_schema.h"
#include "src/references.h"
#include "src/statement_resources.h"
#include "src/table_comments.h"
#include "src/table_metadata.h"
#include "src/table_source.h"
#include "src/temporary_files.h"

namespace bigquery_emulator_duckdb {
namespace {

class DuckDbTableSourceTest : public ::testing::Test {
 protected:
  const TableReference kTable{.project_id = "memory", .dataset_id = "main", .table_id = "external"};
  const std::unordered_map<std::string, std::string> kProjects{{"memory", "memory"}};

  std::unique_ptr<TableSource> Source(Backend& backend,
                                      std::unique_ptr<ExternalTableProvider> provider = {}) {
    return NewDuckDbTableSource(backend, kProjects, nullptr, std::move(provider));
  }

  void CreateExternalTable(Backend& backend) {
    const std::vector<FieldSchema> schema{{.name = "content", .type = FieldType::kString}};
    backend.Execute("CREATE TABLE memory.main.external(content VARCHAR)");
    TableMetadata metadata;
    metadata.external_data =
        GcsExternalData{.uris = {"gs://bucket/source.parquet"}, .autodetect = std::nullopt};
    backend.Execute(TableCommentStatement(kTable, metadata, schema));
  }
};

TEST_F(DuckDbTableSourceTest, MissingProviderRejectsExternalOperations) {
  Backend backend;
  CreateExternalTable(backend);
  const auto source = Source(backend);
  EXPECT_THROW(source->ReadQuery(kTable), ApiError);
  const auto columns = source->DescribeExternalQuery("test.us.connection", "SELECT 1", "");
  EXPECT_EQ(columns.status().code(), absl::StatusCode::kUnimplemented);
}

// A preparation owns its payload instead of borrowing the provider. A real provider retains its
// client and external metadata the same way; the injected source makes the lifetime observable.
class StagedProvider : public ExternalTableProvider {
 public:
  explicit StagedProvider(std::string& path) : path_(path) {}

  TableRead Read(GcsExternalData /*definition*/, std::vector<FieldSchema> /*schema*/) override {
    return [payload = payload_, path = &path_](TemporaryFiles& files) {
      *path = files.Write(*payload);
      return "SELECT content FROM read_text(" + QuoteLiteral(*path) + ")";
    };
  }

  absl::StatusOr<std::vector<FieldSchema>> DescribeQuery(const std::string& /*key*/,
                                                         const std::string& /*query*/,
                                                         const std::string& /*options*/,
                                                         TableSource& /*source*/) override {
    return absl::UnimplementedError("injected source has no connections");
  }

 private:
  std::string& path_;
  std::shared_ptr<const std::string> payload_ = std::make_shared<const std::string>("fixture");
};

TEST_F(DuckDbTableSourceTest, DeferredReadOutlivesProviderAndKeepsFilesWithRows) {
  Backend backend;
  CreateExternalTable(backend);
  std::string path;
  std::optional<TableRead> definition;
  {
    const auto source = Source(backend, std::make_unique<StagedProvider>(path));
    definition = source->ReadQuery(kTable);
    EXPECT_TRUE(path.empty());  // Resolution must not acquire files.
  }
  auto resources = std::make_shared<StatementResources>(StatementResources::Mode::kExecute);
  const auto sql = StatementResources::ReadQuery(kTable, definition, resources.get());
  ASSERT_TRUE(sql.has_value());
  auto result = backend.Execute(*sql);
  result.rows.KeepResources(resources);
  resources.reset();
  definition.reset();
  EXPECT_TRUE(std::filesystem::exists(path));
  ASSERT_EQ(result.rows.size(), 1);
  EXPECT_EQ(result.rows.String(0, 0), "fixture");
  result = {};
  EXPECT_FALSE(std::filesystem::exists(path));
}

}  // namespace
}  // namespace bigquery_emulator_duckdb
