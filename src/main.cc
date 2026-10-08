#include <pthread.h>
#include <unistd.h>

#include <charconv>
#include <csignal>
#include <exception>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#include "src/emulator.h"
#include "src/project.h"
#include "src/server.h"

namespace {

void PrintUsage() {
  std::cerr
      << "Usage: bigquery-emulator-duckdb [--host HOST] [--port PORT] [--data-dir DIR] [--project "
         "JSON|@FILE]... [--session-user ID]\n"
         "  --project JSON|@FILE  Register a project (repeatable); @ reads a JSON file\n"
         "  --session-user ID  Identity returned by SESSION_USER() (default: unsupported)\n"
         "  --host HOST     Address to listen on (default: 0.0.0.0)\n"
         "  --port PORT     Port to listen on (default: 9050)\n"
         "  --data-dir DIR  Store each project in DIR/<project>.duckdb so that data survives\n"
         "                  restarts (default: keep everything in memory)\n";
}

// Blocks SIGINT and SIGTERM in the calling thread and every thread it starts afterwards, so that
// they are only ever received through sigwait() in WaitForShutdown().
sigset_t BlockShutdownSignals() {
  sigset_t signals{};
  sigemptyset(&signals);
  sigaddset(&signals, SIGINT);
  sigaddset(&signals, SIGTERM);
  pthread_sigmask(SIG_BLOCK, &signals, nullptr);
  return signals;
}

// Stops the server on the first shutdown signal. Serve() then returns and the emulator is
// destroyed normally, which checkpoints every project file so that no WAL is left behind.
void WaitForShutdown(const sigset_t& signals, bigquery_emulator_duckdb::Server& server) {
  int signal = 0;
  sigwait(&signals, &signal);
  server.Stop();
}

int Run(int argc, char** argv) {
  bigquery_emulator_duckdb::ServerOptions options;
  std::string data_dir;
  std::optional<std::string> session_user;
  std::vector<bigquery_emulator_duckdb::Project> projects;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    const bool has_value = i + 1 < argc;
    if (arg == "--project" && has_value) {
      projects.push_back(bigquery_emulator_duckdb::ReadProjectArgument(argv[++i]));
    } else if (arg.starts_with("--project=")) {
      projects.push_back(bigquery_emulator_duckdb::ReadProjectArgument(arg.substr(10)));
    } else if (arg == "--session-user" && has_value) {
      session_user = argv[++i];
    } else if (arg.starts_with("--session-user=")) {
      session_user = arg.substr(15);
    } else if (arg == "--host" && has_value) {
      options.host = argv[++i];
    } else if (arg == "--port" && has_value) {
      const std::string_view value = argv[++i];
      const auto [end, error] =
          std::from_chars(value.data(), value.data() + value.size(), options.port);
      if (error != std::errc() || end != value.data() + value.size()) {
        PrintUsage();
        return 2;
      }
    } else if (arg == "--data-dir" && has_value) {
      data_dir = argv[++i];
    } else if (arg == "--help" || arg == "-h") {
      PrintUsage();
      return 0;
    } else {
      PrintUsage();
      return 2;
    }
  }

  const sigset_t signals = BlockShutdownSignals();
  bigquery_emulator_duckdb::Emulator emulator(data_dir, projects, session_user);
  bigquery_emulator_duckdb::Server server(emulator, options);
  if (!server.Bind()) {
    std::cerr << "Failed to bind to " << options.host << ":" << options.port << '\n';
    return 1;
  }
  std::thread shutdown([&] { WaitForShutdown(signals, server); });
  std::cerr << "bigquery-emulator-duckdb listening on " << server.root_url() << '\n';
  std::cerr << "Reading gs:// objects from " << emulator.storage_endpoint() << '\n';
  const bool served = server.Serve();
  // Wakes the shutdown thread when Serve() returned on its own rather than through a signal.
  kill(getpid(), SIGTERM);
  shutdown.join();
  return served ? 0 : 1;
}

}  // namespace

// Everything lives in Run() so that nothing, including building the default ServerOptions,
// can throw out of main().
int main(int argc, char** argv) {
  try {
    return Run(argc, argv);
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  } catch (...) {
    std::cerr << "Unknown error\n";
    return 1;
  }
}
