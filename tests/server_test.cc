#include "src/server.h"

#include <atomic>
#include <latch>
#include <memory>
#include <thread>
#include <vector>

#include "gtest/gtest.h"
#include "src/emulator.h"
#include "src/field_schema.h"
#include "src/httplib.h"
#include "src/table_metadata.h"

namespace bigquery_emulator_duckdb {
namespace {

// REST status codes and response fields are covered by the runbooks in tests/e2e; this covers
// what runn cannot drive, such as many concurrent connections.
class ServerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ServerOptions options;
    options.host = "127.0.0.1";
    options.port = 0;
    server_ = std::make_unique<Server>(emulator_, options);
    ASSERT_TRUE(server_->Bind());
    thread_ = std::thread([this] { server_->Serve(); });
  }

  void TearDown() override {
    server_->Stop();
    thread_.join();
  }

  Emulator emulator_{"", {{.project_id = "p"}}};
  std::unique_ptr<Server> server_;
  std::thread thread_;
};

// Clients such as test suites open many connections at once; with a short listen backlog the
// kernel resets the ones that do not fit instead of queueing them.
TEST_F(ServerTest, AcceptsBurstOfConnections) {
  constexpr int kClients = 100;
  std::latch start(kClients);
  std::atomic<int> failures = 0;
  std::vector<std::thread> clients;
  clients.reserve(kClients);
  for (int i = 0; i < kClients; ++i) {
    clients.emplace_back([&] {
      httplib::Client client(server_->root_url());
      start.arrive_and_wait();
      const httplib::Result result = client.Get("/bigquery/v2/projects/p/datasets");
      if (!result || result->status != 200) {
        ++failures;
      }
    });
  }
  for (std::thread& client : clients) {
    client.join();
  }
  EXPECT_EQ(failures, 0);
}

// Readers arriving together must all see absence, rather than conflicting DROP transactions.
TEST_F(ServerTest, ConcurrentReadsOfExpiredTable) {
  emulator_.CreateDataset({.project_id = "p", .dataset_id = "expired"});
  TableMetadata metadata;
  metadata.expiration_time = 1;
  emulator_.CreateTable({.project_id = "p", .dataset_id = "expired", .table_id = "t"},
                        {{.name = "id", .type = FieldType::kInteger}}, metadata);
  constexpr int kClients = 32;
  std::latch start(kClients);
  std::atomic<int> failures = 0;
  std::vector<std::thread> clients;
  clients.reserve(kClients);
  for (int i = 0; i < kClients; ++i) {
    clients.emplace_back([&] {
      httplib::Client client(server_->root_url());
      start.arrive_and_wait();
      const httplib::Result result =
          client.Get("/bigquery/v2/projects/p/datasets/expired/tables/t");
      if (!result || result->status != 404) {
        ++failures;
      }
    });
  }
  for (std::thread& client : clients) {
    client.join();
  }
  EXPECT_EQ(failures, 0);
}

}  // namespace
}  // namespace bigquery_emulator_duckdb
