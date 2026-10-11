#pragma once

#include <memory>
#include <string>

namespace bigquery_emulator_duckdb {

class Emulator;

// BigQuery Storage v1 transport, sharing the REST server's Emulator. Start binds and starts
// gRPC's workers; Stop cancels active streaming RPCs, then Wait joins them before state dies.
class StorageServer {
 public:
  StorageServer(Emulator& emulator, std::string host, int port);
  ~StorageServer();
  StorageServer(const StorageServer&) = delete;
  StorageServer& operator=(const StorageServer&) = delete;
  bool Start();
  void Stop();
  void Wait();
  [[nodiscard]] int port() const;
  [[nodiscard]] std::string endpoint() const;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace bigquery_emulator_duckdb
