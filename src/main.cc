#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>
#include <string_view>

#include "src/emulator.h"
#include "src/server.h"

namespace {

void PrintUsage() {
  std::cerr << "Usage: bigquery-emulator-duckdb [--host HOST] [--port PORT]\n"
               "  --host HOST  Address to listen on (default: 0.0.0.0)\n"
               "  --port PORT  Port to listen on (default: 9050)\n";
}

int Run(int argc, char** argv) {
  bigquery_emulator_duckdb::ServerOptions options;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    const bool has_value = i + 1 < argc;
    if (arg == "--host" && has_value) {
      options.host = argv[++i];
    } else if (arg == "--port" && has_value) {
      options.port = std::atoi(argv[++i]);
    } else if (arg == "--help" || arg == "-h") {
      PrintUsage();
      return 0;
    } else {
      PrintUsage();
      return 2;
    }
  }

  bigquery_emulator_duckdb::Emulator emulator;
  bigquery_emulator_duckdb::Server server(emulator, options);
  if (!server.Bind()) {
    std::cerr << "Failed to bind to " << options.host << ":" << options.port << '\n';
    return 1;
  }
  std::cerr << "bigquery-emulator-duckdb listening on " << server.root_url() << '\n';
  return server.Serve() ? 0 : 1;
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
