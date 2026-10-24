#include "src/udf/python/worker.h"

#include <fcntl.h>
#include <signal.h>  // NOLINT(modernize-deprecated-headers)
#include <spawn.h>
#include <sys/poll.h>
#include <sys/socket.h>
#include <sys/types.h>  // IWYU pragma: keep -- POSIX provider of pid_t.
#include <sys/wait.h>
#include <unistd.h>

#include "src/udf/python/options.h"
#ifdef __APPLE__
#include <crt_externs.h>
#include <libproc.h>
#include <mach-o/dyld.h>
#include <sys/proc_info.h>
#endif

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iterator>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "nlohmann/json.hpp"
#include "rules_cc/cc/runfiles/runfiles.h"

namespace bigquery_emulator_duckdb::udf::python {
namespace {
using nlohmann::json;
using rules_cc::cc::runfiles::Runfiles;
constexpr size_t kMessageLimit = size_t{64} << 20U;

std::string Executable() {
#ifdef __APPLE__
  uint32_t size = 0;
  _NSGetExecutablePath(nullptr, &size);
  std::string path(size, '\0');
  if (_NSGetExecutablePath(path.data(), &size) != 0) {
    throw std::runtime_error("Cannot locate executable");
  }
  path.resize(path.find('\0'));
  return path;
#else
  std::string path(65536, '\0');
  const auto size = readlink("/proc/self/exe", path.data(), path.size());
  if (size <= 0 || static_cast<size_t>(size) == path.size()) {
    throw std::runtime_error("Cannot locate executable");
  }
  path.resize(static_cast<size_t>(size));
  return path;
#endif
}

}  // namespace

Worker::Worker(PythonOptions options) : options_(std::move(options)) {}

Worker::~Worker() {
  if (socket_ >= 0) {
    close(socket_);
  }
  if (pid_ > 0) {
    // Also reap subprocesses still in the worker's process group. The worker remains an
    // unreaped child until waitpid, so its process ID cannot be reused during cleanup.
    kill(-pid_, SIGKILL);
    kill(pid_, SIGKILL);
    while (waitpid(pid_, nullptr, 0) < 0 && errno == EINTR) {
    }
  }
}

json Worker::Call(const std::string& definition, json arguments) {
  const auto deadline = Clock::now() + options_.call_time;
  if (pid_ < 0) {
    Start();
  }
  const std::string request =
      json{{"definition", definition}, {"arguments", std::move(arguments)}}.dump() + "\n";
  if (request.size() > kMessageLimit) {
    throw std::runtime_error("Python UDF request exceeds 64 MiB");
  }
  size_t offset = 0;
  while (offset < request.size()) {
    Wait(POLLOUT, deadline);
#ifdef __APPLE__
    constexpr int flags = 0;
#else
    constexpr int flags = MSG_NOSIGNAL;
#endif
    const auto count = send(socket_, request.data() + offset, request.size() - offset, flags);
    if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
      continue;
    }
    if (count <= 0) {
      throw std::runtime_error("Python UDF worker exited unexpectedly");
    }
    offset += static_cast<size_t>(count);
  }
  std::string response;
  char buffer[8192];
  while (response.empty() || response.back() != '\n') {
    Wait(POLLIN, deadline);
    const auto count = recv(socket_, buffer, sizeof(buffer), 0);
    if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
      continue;
    }
    if (count <= 0) {
      throw std::runtime_error(
          "Python UDF worker exited unexpectedly (initialization, memory limit or crash)");
    }
    response.append(buffer, static_cast<size_t>(count));
    if (response.size() > kMessageLimit) {
      throw std::runtime_error("Python UDF response exceeds 64 MiB");
    }
  }
  const json result = json::parse(response);
  if (result.contains("error")) {
    throw std::runtime_error(result.at("error").get<std::string>());
  }
  return result.at("value");
}

void Worker::Wait(int16_t events, Clock::time_point deadline) const {
  while (true) {
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
    if (remaining <= 0) {
      throw std::runtime_error("Python UDF timed out");
    }
#ifdef __APPLE__
    // Darwin rejects RLIMIT_AS. Check resident memory while waiting, including during
    // interpreter startup and native calls that do not return to Python.
    proc_taskinfo info{};
    const int size = proc_pidinfo(pid_, PROC_PIDTASKINFO, 0, &info, sizeof(info));
    if (std::cmp_equal(size, sizeof(info)) && info.pti_resident_size > options_.memory) {
      throw std::runtime_error("Python UDF worker memory limit exceeded");
    }
    if (std::cmp_not_equal(size, sizeof(info)) && (size != 0 || errno != ESRCH)) {
      throw std::runtime_error("Cannot read Python worker memory usage");
    }
    const int64_t interval = std::min<int64_t>(remaining, 10);
#else
    const int64_t interval = remaining;
#endif
    pollfd descriptor{.fd = socket_, .events = events, .revents = 0};
    const int ready =
        poll(&descriptor, 1,
             static_cast<int>(std::min<int64_t>(interval, std::numeric_limits<int>::max())));
    if (ready < 0 && errno == EINTR) {
      continue;
    }
    if (ready < 0) {
      throw std::runtime_error("Python worker poll failed");
    }
    if (ready > 0) {
      return;
    }
  }
}

void Worker::Start() {
  const std::string executable = Executable();
  std::unique_ptr<Runfiles> const files(Runfiles::Create(executable));
  std::string worker = options_.worker;
  if (worker.empty() && files) {
    worker = files->Rlocation("_main/python_udf_worker");
  }
  if (worker.empty() || (!std::filesystem::exists(worker) && options_.worker.empty())) {
    worker = (std::filesystem::path(executable).parent_path() / "python_udf_worker").string();
  }
  std::vector<std::string> environment;
#ifdef __APPLE__
  char** const inherited = *_NSGetEnviron();
#else
  char** const inherited = environ;
#endif
  for (char** item = inherited; *item != nullptr; ++item) {
    const std::string entry = *item;
    if (!entry.starts_with("RUNFILES_DIR=") && !entry.starts_with("RUNFILES_MANIFEST_FILE=") &&
        !entry.starts_with("JAVA_RUNFILES=")) {
      environment.push_back(entry);
    }
  }
  if (files) {
    for (const auto& [key, value] : files->EnvVars()) {
      environment.push_back(key);
      environment.back().append("=").append(value);
    }
  }
  std::vector<char*> env;
  std::ranges::transform(environment, std::back_inserter(env),
                         // POSIX promises not to modify envp, although its signature takes char*.
                         [](const std::string& item) { return const_cast<char*>(item.c_str()); });
  env.push_back(nullptr);
  int sockets[2] = {-1, -1};
#ifdef __APPLE__
  constexpr int socket_type = SOCK_STREAM;
#else
  constexpr int socket_type = SOCK_STREAM | SOCK_CLOEXEC;
#endif
  if (socketpair(AF_UNIX, socket_type, 0, sockets) < 0) {
    throw std::runtime_error("Cannot create Python worker socket");
  }
  socket_ = sockets[0];
#ifdef __APPLE__
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) -- POSIX integer flag argument.
  fcntl(sockets[0], F_SETFD, FD_CLOEXEC);
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) -- POSIX integer flag argument.
  fcntl(sockets[1], F_SETFD, FD_CLOEXEC);
  const int no_sigpipe = 1;
  setsockopt(socket_, SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe, sizeof(no_sigpipe));
#endif
  posix_spawn_file_actions_t actions{};
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_addclose(&actions, sockets[0]);
  posix_spawn_file_actions_adddup2(&actions, sockets[1], 3);
  if (sockets[1] != 3) {
    posix_spawn_file_actions_addclose(&actions, sockets[1]);
  }
  for (int fd = 0; fd < 3; ++fd) {
    posix_spawn_file_actions_addopen(&actions, fd, "/dev/null", O_RDWR, 0);
  }
  posix_spawnattr_t attributes{};
  posix_spawnattr_init(&attributes);
  sigset_t mask{};
  sigemptyset(&mask);
  posix_spawnattr_setsigmask(&attributes, &mask);
  posix_spawnattr_setpgroup(&attributes, 0);
  posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETPGROUP);
  std::string memory = std::to_string(options_.memory);
  char* args[] = {worker.data(), memory.data(), nullptr};
  const int error = posix_spawn(&pid_, worker.c_str(), &actions, &attributes, args, env.data());
  posix_spawnattr_destroy(&attributes);
  posix_spawn_file_actions_destroy(&actions);
  close(sockets[1]);
  if (error != 0) {
    pid_ = -1;
    throw std::runtime_error("Cannot start Python UDF worker: " +
                             std::system_category().message(error));
  }
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) -- POSIX integer flag argument.
  if (fcntl(socket_, F_SETFL, O_NONBLOCK) < 0) {
    throw std::runtime_error("Cannot configure Python worker socket");
  }
}

}  // namespace bigquery_emulator_duckdb::udf::python
