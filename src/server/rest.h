#pragma once

#include <memory>
#include <string>

namespace bigquery_emulator_duckdb {

class Emulator;

// BigQuery v2 REST transport. Server owns its serving thread and lifecycle.
class RestServer {
 public:
  RestServer(Emulator& emulator, std::string host, int port);
  ~RestServer();
  RestServer(const RestServer&) = delete;
  RestServer& operator=(const RestServer&) = delete;

  bool Bind();
  bool Serve();
  bool WaitUntilReady();
  void Stop();
  [[nodiscard]] int port() const { return port_; }
  [[nodiscard]] std::string root_url() const;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
  std::string host_;
  int requested_port_;
  int port_ = 0;
};

}  // namespace bigquery_emulator_duckdb
