#pragma once

#include <memory>
#include <string>

namespace bigquery_emulator_duckdb {

class Emulator;

struct ServerOptions {
  std::string host = "0.0.0.0";
  int port = 9050;
};

// Owns API transports and their serving threads, sharing one Emulator.
// The Emulator must outlive the Server.
class Server {
 public:
  Server(Emulator& emulator, ServerOptions options);
  ~Server();

  Server(const Server&) = delete;
  Server& operator=(const Server&) = delete;

  // Starts serving and waits until the listener is ready. Port 0 chooses an ephemeral port.
  // Returns false on bind failure or if Start() was already called; instances are single-use.
  // Call Start() and Wait() from the owning thread. Stop() may be called by other threads
  // after Start() returns. Destruction stops and joins all serving threads.
  bool Start();
  // Waits for serving to finish; returns false on transport failure and propagates exceptions.
  bool Wait();
  // Requests shutdown. Safe to call repeatedly, including from concurrent threads.
  void Stop();

  [[nodiscard]] int port() const;
  // The URL clients should use as the API root, e.g. http://127.0.0.1:9050
  [[nodiscard]] std::string root_url() const;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace bigquery_emulator_duckdb
