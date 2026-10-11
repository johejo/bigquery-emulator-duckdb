#include "src/server.h"

#include <exception>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

#include "src/server/rest.h"
#include "src/server/storage.h"

namespace bigquery_emulator_duckdb {

class Server::Impl {
 public:
  Impl(Emulator& emulator, ServerOptions options)
      : rest(emulator, options.host, options.port),
        storage(emulator, std::move(options.host), options.grpc_port) {}

  RestServer rest;
  StorageServer storage;
  std::thread worker;
  std::mutex stop_mutex;
  bool started = false;
  bool stopped = false;
  bool served = false;
  std::exception_ptr failure;
};

Server::Server(Emulator& emulator, ServerOptions options)
    : impl_(std::make_unique<Impl>(emulator, std::move(options))) {}

Server::~Server() {
  Stop();
  if (impl_->worker.joinable()) {
    impl_->worker.join();
  }
}

bool Server::Start() {
  if (impl_->started) {
    return false;
  }
  impl_->started = true;
  // httplib cannot close a bound socket through stop() until its serving loop starts.
  // Start gRPC first so a failed gRPC bind cannot leave an unserved REST socket behind.
  if (!impl_->storage.Start()) {
    return false;
  }
  if (!impl_->rest.Bind()) {
    impl_->storage.Stop();
    impl_->storage.Wait();
    return false;
  }
  impl_->worker = std::thread([this] {
    try {
      impl_->served = impl_->rest.Serve();
    } catch (...) {
      impl_->failure = std::current_exception();
    }
  });
  if (!impl_->rest.WaitUntilReady()) {
    Wait();
    return false;
  }
  return true;
}

bool Server::Wait() {
  if (impl_->worker.joinable()) {
    impl_->worker.join();
  }
  impl_->storage.Stop();
  impl_->storage.Wait();
  if (impl_->failure) {
    std::rethrow_exception(impl_->failure);
  }
  return impl_->served;
}

void Server::Stop() {
  std::scoped_lock const lock(impl_->stop_mutex);
  if (!impl_->started || impl_->stopped) {
    return;
  }
  impl_->stopped = true;
  impl_->rest.Stop();
  impl_->storage.Stop();
}

int Server::port() const { return impl_->rest.port(); }

std::string Server::root_url() const { return impl_->rest.root_url(); }

int Server::grpc_port() const { return impl_->storage.port(); }
std::string Server::grpc_endpoint() const { return impl_->storage.endpoint(); }

}  // namespace bigquery_emulator_duckdb
