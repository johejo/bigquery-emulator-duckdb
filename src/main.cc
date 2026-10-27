// include-cleaner requires signal.h for POSIX APIs such as sigwait and pthread_sigmask;
// modernize-deprecated-headers suggests csignal instead, so suppress it on this include.
#include <signal.h>  // NOLINT(modernize-deprecated-headers)
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "CLI/CLI.hpp"
#include "src/backend_initializer.h"
#include "src/builtin_function_options.h"
#include "src/emulator.h"
#include "src/external_query.h"
#include "src/googlesql_functions_duckdb/options.h"
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

constexpr const char* kUdfs = "UDF limits";
constexpr const char* kDebugging = "Debugging";

// What both the server and the shell configure.
struct EmulatorFlags {
  std::string data_dir;
  std::optional<std::string> session_user;
  bigquery_emulator_duckdb::udf::JavaScriptLimits javascript;
  bigquery_emulator_duckdb::udf::PythonOptions python;
  std::vector<std::string> projects;
  std::vector<std::string> connections;
};

void AddEmulatorFlags(CLI::App& app, EmulatorFlags& flags) {
  // An empty description keeps the bounds out of the help's type names.
  const CLI::Validator positive =
      CLI::Range(uint32_t{1}, std::numeric_limits<uint32_t>::max()).description("");
  app.add_option("--project", flags.projects,
                 "Register a project, as JSON or @FILE to read a JSON file (repeatable)")
      ->type_name("JSON|@FILE")
      ->allow_extra_args(false)
      ->take_all();
  app.add_option("--connection", flags.connections,
                 "Let EXTERNAL_QUERY reach PostgreSQL, MySQL or Spanner (libpq connection "
                 "string/URI or spanner://projects/P/instances/I/databases/D), or OBJ functions "
                 "read Cloud Storage (cloud_resource), as the connection ID "
                 "PROJECT.LOCATION.CONNECTION (repeatable)")
      ->type_name("ID=CONNINFO")
      ->allow_extra_args(false)
      ->take_all();
  app.add_option("--data-dir", flags.data_dir,
                 "Store each project in DIR/<project>.duckdb so that data survives restarts "
                 "(default: keep everything in memory)")
      ->type_name("DIR");
  // Binding std::optional directly would turn an empty value into an unset one.
  app.add_option_function<std::string>(
         "--session-user",
         [&flags](const std::string& value) {
           if (value.empty()) {
             throw CLI::ValidationError("--session-user", "must not be empty");
           }
           flags.session_user = value;
         },
         "Identity returned by SESSION_USER() (default: unsupported)")
      ->type_name("ID");
  app.add_option_function<uint32_t>(
         "--javascript-timeout-ms",
         [&flags](uint32_t value) {
           flags.javascript.call_time = std::chrono::milliseconds(value);
         },
         "How long one JavaScript UDF call may run (default: 300000)")
      ->type_name("MS")
      ->check(positive)
      ->group(kUdfs);
  app.add_option_function<uint32_t>(
         "--javascript-memory-mib",
         [&flags](uint32_t value) { flags.javascript.memory = size_t{value} << 20U; },
         "How much memory JavaScript UDF calls may allocate per query on each thread "
         "(default: 256)")
      ->type_name("MIB")
      ->check(positive)
      ->group(kUdfs);
  app.add_option("--python-worker", flags.python.worker,
                 "Override the Python worker with an external executable")
      ->type_name("PATH")
      ->group(kDebugging);
  app.add_option_function<uint32_t>(
         "--python-timeout-ms",
         [&flags](uint32_t value) { flags.python.call_time = std::chrono::milliseconds(value); },
         "Python UDF call timeout, including worker startup (default: 300000)")
      ->type_name("MS")
      ->check(positive)
      ->group(kUdfs);
  app.add_option_function<uint32_t>(
         "--python-memory-mib",
         [&flags](uint32_t value) { flags.python.memory = size_t{value} << 20U; },
         "Python worker memory limit (default: 256)")
      ->type_name("MIB")
      ->check(positive)
      ->group(kUdfs);
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

int Run(int argc, char* const* argv) {
  CLI::App app("Emulates BigQuery backed by DuckDB.", "bigquery-emulator-duckdb");
  app.require_subcommand(1);
  // A repeated flag takes its last value, except where it is repeatable; subcommands inherit this.
  app.option_defaults()->multi_option_policy(CLI::MultiOptionPolicy::TakeLast);

  CLI::App* serve =
      app.add_subcommand("serve", "Serve the BigQuery REST API and BigQuery Storage API");
  EmulatorFlags serve_flags;
  AddEmulatorFlags(*serve, serve_flags);
  bigquery_emulator_duckdb::ServerOptions server_options;
  const CLI::Validator port = CLI::Range(0, 65535).description("");
  serve->add_option("--host", server_options.host, "Address to listen on (default: 0.0.0.0)")
      ->type_name("HOST");
  serve->add_option("--http-port", server_options.port, "REST API port (default: 9050)")
      ->type_name("PORT")
      ->check(port);
  serve
      ->add_option("--grpc-port", server_options.grpc_port,
                   "BigQuery Storage gRPC port (default: 9060)")
      ->type_name("PORT")
      ->check(port);
  serve->footer("Port 0 chooses a free port.");

  CLI::App* shell = app.add_subcommand("shell", "Run queries in the process without listening");
  EmulatorFlags shell_flags;
  AddEmulatorFlags(*shell, shell_flags);
  bigquery_emulator_duckdb::ShellOptions shell_options;
  shell
      ->add_option("--default-project", shell_options.default_project,
                   "Project that runs queries (default: the only registered project)")
      ->type_name("ID");
  shell
      ->add_option("--default-dataset", shell_options.default_dataset,
                   "Dataset that unqualified table names refer to")
      ->type_name("DATASET");
  shell
      ->add_option_function<std::string>(
          "--format",
          [&shell_options](const std::string& value) {
            shell_options.format = bigquery_emulator_duckdb::shell::ParseOutputFormat(value);
            if (!shell_options.format.has_value()) {
              throw CLI::ValidationError("--format", "must be pretty, json, jsonl or csv");
            }
          },
          "pretty, json, jsonl or csv (default: pretty on a terminal, otherwise jsonl)")
      ->type_name("FORMAT");
  shell
      ->add_option_function<std::string>(
          "-e,--execute",
          [&shell_options](const std::string& value) {
            if (value.find_first_not_of(" \t\r\n") == std::string::npos) {
              throw CLI::ValidationError("--execute", "must not be empty");
            }
            shell_options.execute = value;
          },
          "Run SQL as one job instead of reading standard input")
      ->type_name("SQL");
  bool googlesql_semantics = false;
  shell
      ->add_option_function<std::string>(
          "--semantics",
          [&googlesql_semantics](const std::string& value) {
            if (value != "bigquery" && value != "googlesql") {
              throw CLI::ValidationError("--semantics", "must be bigquery or googlesql");
            }
            googlesql_semantics = value == "googlesql";
          },
          "bigquery, or googlesql to evaluate functions as GoogleSQL's reference "
          "implementation does, without BigQuery's differences (default: bigquery)")
      ->type_name("SEMANTICS")
      ->group(kDebugging);
  shell
      ->add_flag("--duckdb", shell_options.duckdb,
                 "Run SQL as DuckDB statements on the emulator's database, where "
                 "bq_geography_text(column) shows a stored GEOGRAPHY")
      ->group(kDebugging);

  try {
    app.parse(argc, argv);
  } catch (const CLI::ParseError& error) {
    return app.exit(error) == 0 ? 0 : 2;
  }

  const EmulatorFlags& used = shell->parsed() ? shell_flags : serve_flags;
  std::vector<bigquery_emulator_duckdb::Project> projects;
  std::ranges::transform(used.projects, std::back_inserter(projects),
                         bigquery_emulator_duckdb::ReadProjectArgument);
  std::vector<bigquery_emulator_duckdb::ExternalConnection> connections;
  std::ranges::transform(used.connections, std::back_inserter(connections),
                         bigquery_emulator_duckdb::ParseConnectionArgument);
  const bigquery_emulator_duckdb::BackendInitializer udfs =
      bigquery_emulator_duckdb::udf_duckdb::UdfInitializer(used.javascript, used.python);

  if (shell->parsed()) {
    if (googlesql_semantics) {
      // GoogleSQL does not implement GEOGRAPHY, so its bounds have no BigQuery adjustment either.
      const googlesql_geography::Options geography;
      const bigquery_emulator_duckdb::functions_duckdb::Options functions;
      bigquery_emulator_duckdb::Emulator emulator(
          used.data_dir, projects, used.session_user, udfs, connections, geography, functions,
          bigquery_emulator_duckdb::ExcludedFunctionIds(),
          bigquery_emulator_duckdb::FunctionDeclarations::kGoogleSQL);
      return bigquery_emulator_duckdb::RunShell(emulator, shell_options);
    }
    bigquery_emulator_duckdb::Emulator emulator(used.data_dir, projects, used.session_user, udfs,
                                                connections);
    return bigquery_emulator_duckdb::RunShell(emulator, shell_options);
  }

  const sigset_t signals = BlockShutdownSignals();
  bigquery_emulator_duckdb::Emulator emulator(used.data_dir, projects, used.session_user, udfs,
                                              connections);
  bigquery_emulator_duckdb::Server server(emulator, server_options);
  if (!server.Start()) {
    std::cerr << "Failed to bind REST " << server_options.host << ":" << server_options.port
              << " or gRPC " << server_options.host << ":" << server_options.grpc_port << '\n';
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
