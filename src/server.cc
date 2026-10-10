#include "src/server.h"

#include <exception>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

#include "src/server/rest.h"

namespace bigquery_emulator_duckdb {

class Server::Impl {
 public:
  Impl(Emulator& emulator, ServerOptions options)
      : rest(emulator, std::move(options.host), options.port) {}

  RestServer rest;
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
  if (!impl_->rest.Bind()) {
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
}

int Server::port() const { return impl_->rest.port(); }

std::string Server::root_url() const { return impl_->rest.root_url(); }

}  // namespace bigquery_emulator_duckdb
