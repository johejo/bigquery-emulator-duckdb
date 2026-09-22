#pragma once

#include <memory>
#include <string>

namespace bigquery_emulator_duckdb {

class Emulator;

struct ServerOptions {
  std::string host = "0.0.0.0";
  int port = 9050;
};

// HTTP server implementing the subset of the BigQuery v2 REST API that the emulator supports.
class Server {
 public:
  Server(Emulator& emulator, ServerOptions options);
  ~Server();

  Server(const Server&) = delete;
  Server& operator=(const Server&) = delete;

  // Binds to the configured host and port. When `options.port` is 0, an ephemeral port is
  // chosen and can be read back with `port()`. Returns false when binding fails.
  bool Bind();
  // Serves requests until `Stop()` is called. `Bind()` must have succeeded.
  bool Serve();
  void Stop();

  int port() const { return port_; }
  // The URL clients should use as the API root, e.g. http://127.0.0.1:9050
  std::string root_url() const;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
  ServerOptions options_;
  int port_ = 0;
};

}  // namespace bigquery_emulator_duckdb
