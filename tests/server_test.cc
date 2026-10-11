#include "src/server.h"

#include <atomic>
#include <chrono>
#include <latch>
#include <memory>
#include <thread>
#include <vector>

#include "google/cloud/bigquery/storage/v1/storage.grpc.pb.h"
#include "google/cloud/bigquery/storage/v1/storage.pb.h"
#include "grpcpp/grpcpp.h"
#include "grpcpp/security/credentials.h"
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
    options.grpc_port = 0;
    server_ = std::make_unique<Server>(emulator_, options);
    ASSERT_TRUE(server_->Start());
  }

  void TearDown() override {
    server_->Stop();
    EXPECT_TRUE(server_->Wait());
  }

  Emulator emulator_{"", {{.project_id = "p"}}};
  std::unique_ptr<Server> server_;
};

TEST(ServerLifecycleTest, StopImmediatelyAfterStart) {
  Emulator emulator;
  Server server(emulator, {.host = "127.0.0.1", .port = 0, .grpc_port = 0});
  server.Stop();
  ASSERT_TRUE(server.Start());
  EXPECT_GT(server.port(), 0);
  server.Stop();
  EXPECT_TRUE(server.Wait());
  server.Stop();
  EXPECT_TRUE(server.Wait());
  EXPECT_FALSE(server.Start());
}

TEST(ServerLifecycleTest, DestructorStopsAndReleasesListener) {
  Emulator emulator;
  int port = 0;
  {
    Server server(emulator, {.host = "127.0.0.1", .port = 0, .grpc_port = 0});
    ASSERT_TRUE(server.Start());
    port = server.port();
  }
  Server next(emulator, {.host = "127.0.0.1", .port = port, .grpc_port = 0});
  ASSERT_TRUE(next.Start());
}

TEST(ServerLifecycleTest, BindFailureDoesNotAffectOtherServer) {
  Emulator emulator;
  Server first(emulator, {.host = "127.0.0.1", .port = 0, .grpc_port = 0});
  ASSERT_TRUE(first.Start());
  Server second(emulator, {.host = "127.0.0.1", .port = first.port(), .grpc_port = 0});
  ASSERT_FALSE(second.Start());
  EXPECT_FALSE(second.Wait());
  ASSERT_GT(second.grpc_port(), 0);
  Server third(emulator, {.host = "127.0.0.1", .port = 0, .grpc_port = second.grpc_port()});
  EXPECT_TRUE(third.Start());
  httplib::Client client(first.root_url());
  EXPECT_TRUE(client.Get("/bigquery/v2/projects"));
}

TEST(ServerLifecycleTest, RejectsOutOfRangePorts) {
  Emulator emulator;
  for (const int port : {-1, 65536}) {
    Server server(emulator, {.host = "127.0.0.1", .port = port, .grpc_port = 0});
    EXPECT_FALSE(server.Start());
  }
}

TEST(ServerLifecycleTest, GrpcBindFailureDoesNotBindRestListener) {
  Emulator emulator;
  Server first(emulator, {.host = "127.0.0.1", .port = 0, .grpc_port = 0});
  ASSERT_TRUE(first.Start());
  ASSERT_GT(first.grpc_port(), 0);
  Server second(emulator, {.host = "127.0.0.1", .port = 0, .grpc_port = first.grpc_port()});
  EXPECT_FALSE(second.Start());
  EXPECT_EQ(second.port(), 0);
  Server third(emulator, {.host = "127.0.0.1", .port = second.port(), .grpc_port = 0});
  EXPECT_TRUE(third.Start());
}

TEST_F(ServerTest, ShutdownCancelsIdleAppendConnection) {
  namespace pb = google::cloud::bigquery::storage::v1;
  emulator_.CreateDataset({.project_id = "p", .dataset_id = "grpc"});
  emulator_.CreateTable({.project_id = "p", .dataset_id = "grpc", .table_id = "t"},
                        {{.name = "id", .type = FieldType::kInteger}});
  const auto channel =
      grpc::CreateChannel(server_->grpc_endpoint(), grpc::InsecureChannelCredentials());
  const auto client = pb::BigQueryWrite::NewStub(channel);
  grpc::ClientContext context;
  context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(15));
  const auto connection = client->AppendRows(&context);
  pb::AppendRowsRequest request;
  request.set_write_stream("projects/p/datasets/grpc/tables/t/streams/_default");
  request.mutable_proto_rows()->mutable_writer_schema()->mutable_proto_descriptor()->set_name(
      "Row");
  ASSERT_TRUE(connection->Write(request));
  pb::AppendRowsResponse response;
  ASSERT_TRUE(connection->Read(&response));
  ASSERT_FALSE(response.has_error());
  server_->Stop();
  EXPECT_TRUE(server_->Wait());
  EXPECT_FALSE(connection->Read(&response));
  EXPECT_FALSE(connection->Finish().ok());
}

TEST(ServerLifecycleTest, FormatsIPv6Endpoint) {
  Emulator emulator;
  const Server wildcard(emulator, {.host = "::", .port = 0, .grpc_port = 0});
  EXPECT_EQ(wildcard.root_url(), "http://[::1]:0");
  const Server loopback(emulator, {.host = "::1", .port = 0, .grpc_port = 0});
  EXPECT_EQ(loopback.root_url(), "http://[::1]:0");
}

TEST_F(ServerTest, ConcurrentStop) {
  constexpr int kCallers = 16;
  std::latch start(kCallers);
  std::vector<std::thread> callers;
  callers.reserve(kCallers);
  for (int i = 0; i < kCallers; ++i) {
    callers.emplace_back([this, &start] {
      start.arrive_and_wait();
      server_->Stop();
    });
  }
  for (std::thread& caller : callers) {
    caller.join();
  }
  EXPECT_TRUE(server_->Wait());
}

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
