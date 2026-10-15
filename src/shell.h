#pragma once

#include <optional>
#include <string>

#include "src/shell/format.h"

namespace bigquery_emulator_duckdb {

class Emulator;

struct ShellOptions {
  // Empty uses the only registered project.
  std::string project_id;
  std::string dataset_id;
  // Unset prints a table to a terminal and JSON lines otherwise.
  std::optional<shell::OutputFormat> format;
  // Runs this query instead of reading standard input.
  std::optional<std::string> execute;
};

// Runs queries from `options.execute` or standard input as query jobs and prints their results to
// standard output. An interactive input runs as one job per entry, ended by a semicolon after a
// complete script; other input runs as one job. Returns the process's exit code.
int RunShell(Emulator& emulator, const ShellOptions& options);

}  // namespace bigquery_emulator_duckdb
