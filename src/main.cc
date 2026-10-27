// include-cleaner requires signal.h for POSIX APIs such as sigwait and pthread_sigmask;
// modernize-deprecated-headers suggests csignal instead, so suppress it on this include.
#include <signal.h>  // NOLINT(modernize-deprecated-headers)
#include <unistd.h>

#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#include "src/emulator.h"
#include "src/external_query.h"
#include "src/googlesql_geography/options.h"
#include "src/project.h"
#include "src/python_worker.h"
#include "src/server.h"
#include "src/shell.h"
#include "src/shell/format.h"
#include "src/udf/javascript/limits.h"
#include "src/udf/python/options.h"
#include "src/udf_duckdb/register.h"

namespace {

void PrintUsage() {
  std::cerr
      << "Usage: bigquery-emulator-duckdb [--host HOST] [--port PORT] [--grpc-port PORT] "
         "[--data-dir DIR] [--project "
         "JSON|@FILE]... [--session-user ID] [UDF OPTIONS]\n"
         "       [--connection ID=CONNINFO]...\n"
         "       bigquery-emulator-duckdb shell [--project-id ID] [--dataset-id DATASET] "
         "[--format FORMAT] [-e SQL] [--data-dir DIR] [--project JSON|@FILE]... "
         "[--session-user ID] [UDF OPTIONS] [--expand-bounding-box true|false] [--duckdb]\n"
         "       [--connection ID=CONNINFO]...\n"
         "  --project JSON|@FILE  Register a project (repeatable); @ reads a JSON file\n"
         "  --session-user ID  Identity returned by SESSION_USER() (default: unsupported)\n"
         "  --javascript-timeout-ms MS  How long one JavaScript UDF call may run (default:\n"
         "                              300000)\n"
         "  --javascript-memory-mib MIB  How much memory JavaScript UDF calls may allocate per\n"
         "                               query on each thread (default: 256)\n"
         "  --python-worker PATH  Override the Python worker with an external executable\n"
         "  --python-timeout-ms MS  Python UDF call timeout, including worker startup (default: "
         "300000)\n"
         "  --python-memory-mib MIB  Python worker memory limit (default: 256)\n"
         "  --connection ID=CONNINFO  Let EXTERNAL_QUERY reach PostgreSQL, MySQL or Spanner\n"
         "                  (libpq connection string/URI or "
         "spanner://projects/P/instances/I/databases/D),\n"
         "                  or OBJ functions read Cloud Storage (cloud_resource),\n"
         "                  as the connection ID,\n"
         "                  PROJECT.LOCATION.CONNECTION (repeatable)\n"
         "  --host HOST     Address to listen on (default: 0.0.0.0)\n"
         "  --port PORT     Port to listen on (default: 9050)\n"
         "  --grpc-port PORT  BigQuery Storage gRPC port (default: 9060; 0 chooses a free port)\n"
         "  --data-dir DIR  Store each project in DIR/<project>.duckdb so that data survives\n"
         "                  restarts (default: keep everything in memory)\n"
         "shell runs queries from standard input without listening:\n"
         "  --project-id ID  Project that runs queries (default: the only registered project)\n"
         "  --dataset-id DATASET  Default dataset\n"
         "  --format FORMAT  pretty, json, jsonl or csv (default: pretty on a terminal, otherwise\n"
         "                   jsonl)\n"
         "  --expand-bounding-box true|false  Round geography bounds outward as BigQuery does\n"
         "                                    (default: true; shell only)\n"
         "  -e SQL           Run SQL as one job instead of reading standard input\n"
         "  --duckdb         Run SQL as DuckDB statements on the emulator's database, where\n"
         "                   bq_geography_text(column) shows a stored GEOGRAPHY (for debugging)\n";
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

// Stops the server on the first shutdown signal. Wait() then returns and the emulator is
// destroyed normally, which checkpoints every project file so that no WAL is left behind.
void WaitForShutdown(const sigset_t& signals, bigquery_emulator_duckdb::Server& server) {
  int signal = 0;
  sigwait(&signals, &signal);
  server.Stop();
}

// A positive decimal that fits in uint32_t, or nullopt.
std::optional<uint32_t> ParsePositive(std::string_view value) {
  uint32_t number = 0;
  const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), number);
  if (error != std::errc() || end != value.data() + value.size() || number == 0) {
    return std::nullopt;
  }
  return number;
}

int Run(int argc, char* const* argv) {
  const bool shell = argc > 1 && std::string_view(argv[1]) == "shell";
  bigquery_emulator_duckdb::ServerOptions options;
  bigquery_emulator_duckdb::ShellOptions shell_options;
  std::string data_dir;
  googlesql_geography::Options geography{.expand_bounding_box = true};
  std::optional<std::string> session_user;
  bigquery_emulator_duckdb::udf::JavaScriptLimits javascript;
  bigquery_emulator_duckdb::udf::PythonOptions python;
  std::vector<bigquery_emulator_duckdb::Project> projects;
  std::vector<bigquery_emulator_duckdb::ExternalConnection> connections;
  for (int i = shell ? 2 : 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    const bool has_value = i + 1 < argc;
    if (arg == "--project" && has_value) {
      projects.push_back(bigquery_emulator_duckdb::ReadProjectArgument(argv[++i]));
    } else if (arg.starts_with("--project=")) {
      projects.push_back(bigquery_emulator_duckdb::ReadProjectArgument(arg.substr(10)));
    } else if (arg == "--connection" && has_value) {
      connections.push_back(bigquery_emulator_duckdb::ParseConnectionArgument(argv[++i]));
    } else if (arg.starts_with("--connection=")) {
      connections.push_back(bigquery_emulator_duckdb::ParseConnectionArgument(arg.substr(13)));
    } else if (arg == "--session-user" && has_value) {
      session_user = argv[++i];
    } else if (arg.starts_with("--session-user=")) {
      session_user = arg.substr(15);
    } else if (arg == "--python-worker" && has_value) {
      python.worker = argv[++i];
    } else if (arg.starts_with("--python-worker=")) {
      python.worker = arg.substr(16);
    } else if (const std::string_view name = arg.substr(0, arg.find('='));
               name == "--javascript-timeout-ms" || name == "--javascript-memory-mib" ||
               name == "--python-timeout-ms" || name == "--python-memory-mib") {
      std::optional<uint32_t> value;
      if (name.size() < arg.size()) {
        value = ParsePositive(arg.substr(name.size() + 1));
      } else if (has_value) {
        value = ParsePositive(argv[++i]);
      }
      if (!value) {
        PrintUsage();
        return 2;
      }
      if (name == "--javascript-timeout-ms") {
        javascript.call_time = std::chrono::milliseconds(*value);
      } else if (name == "--python-timeout-ms") {
        python.call_time = std::chrono::milliseconds(*value);
      } else if (name == "--python-memory-mib") {
        python.memory = size_t{*value} << 20U;
      } else {
        javascript.memory = size_t{*value} << 20U;
      }
    } else if (const std::string_view option_name = arg.substr(0, arg.find('='));
               shell && option_name == "--expand-bounding-box") {
      std::string_view value;
      if (option_name.size() < arg.size()) {
        value = arg.substr(option_name.size() + 1);
      } else if (has_value) {
        value = argv[++i];
      }
      if (value != "true" && value != "false") {
        PrintUsage();
        return 2;
      }
      geography.expand_bounding_box = value == "true";
    } else if (shell && arg == "--project-id" && has_value) {
      shell_options.project_id = argv[++i];
    } else if (shell && arg == "--dataset-id" && has_value) {
      shell_options.dataset_id = argv[++i];
    } else if (shell && arg == "--format" && has_value) {
      shell_options.format = bigquery_emulator_duckdb::shell::ParseOutputFormat(argv[++i]);
      if (!shell_options.format.has_value()) {
        PrintUsage();
        return 2;
      }
    } else if (shell && arg == "-e" && has_value) {
      shell_options.execute = argv[++i];
    } else if (shell && arg == "--duckdb") {
      shell_options.duckdb = true;
    } else if (!shell && arg == "--host" && has_value) {
      options.host = argv[++i];
    } else if (!shell && (arg == "--port" || arg == "--grpc-port") && has_value) {
      const std::string_view value = argv[++i];
      int& port = arg == "--port" ? options.port : options.grpc_port;
      const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), port);
      if (error != std::errc() || end != value.data() + value.size() || port < 0 || port > 65535) {
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

  if (shell) {
    bigquery_emulator_duckdb::Emulator emulator(
        data_dir, projects, session_user,
        bigquery_emulator_duckdb::udf_duckdb::UdfInitializer(javascript, python), connections,
        geography);
    return bigquery_emulator_duckdb::RunShell(emulator, shell_options);
  }

  const sigset_t signals = BlockShutdownSignals();
  bigquery_emulator_duckdb::Emulator emulator(
      data_dir, projects, session_user,
      bigquery_emulator_duckdb::udf_duckdb::UdfInitializer(javascript, python), connections,
      geography);
  bigquery_emulator_duckdb::Server server(emulator, options);
  if (!server.Start()) {
    std::cerr << "Failed to bind REST " << options.host << ":" << options.port << " or gRPC "
              << options.host << ":" << options.grpc_port << '\n';
    return 1;
  }
  std::thread shutdown([&] { WaitForShutdown(signals, server); });
  bool served = false;
  std::exception_ptr failure;
  try {
    std::cerr << "BigQuery Storage gRPC listening on " << server.grpc_endpoint() << '\n';
    std::cerr << "bigquery-emulator-duckdb listening on " << server.root_url() << '\n';
    std::cerr << "Reading gs:// objects from " << emulator.storage_endpoint() << '\n';
    served = server.Wait();
  } catch (...) {
    failure = std::current_exception();
    server.Stop();
  }
  // Wakes the shutdown thread when Wait() returned on its own rather than through a signal.
  kill(getpid(), SIGTERM);
  shutdown.join();
  if (failure) {
    std::rethrow_exception(failure);
  }
  return served ? 0 : 1;
}

}  // namespace

// Everything lives in Run() so that nothing, including building the default ServerOptions,
// can throw out of main().
int main(int argc, char** argv) {
  if (argc > 1 && std::string_view(argv[1]) == "internal-python-worker") {
    // Keep argv[0] for runtime discovery, and retain the external worker's memory argument.
    if (argc != 3) {
      return 2;
    }
    char* worker_argv[] = {argv[0], argv[2], nullptr};
    return bigquery_emulator_duckdb::PythonWorkerMain(2, worker_argv);
  }
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
