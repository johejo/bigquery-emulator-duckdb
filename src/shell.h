#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace bigquery_emulator_duckdb {

class Emulator;

enum class OutputFormat : std::uint8_t { kPretty, kJson, kJsonl, kCsv };

std::optional<OutputFormat> ParseOutputFormat(std::string_view name);

struct ShellOptions {
  // Empty uses the only registered project.
  std::string project_id;
  std::string dataset_id;
  // Unset prints a table to a terminal and JSON lines otherwise.
  std::optional<OutputFormat> format;
  // Runs this query instead of reading standard input.
  std::optional<std::string> execute;
};

// Runs queries from `options.execute` or standard input as query jobs and prints their results to
// standard output. An interactive input runs as one job per entry, ended by a semicolon after a
// complete script; other input runs as one job. Returns the process's exit code.
int RunShell(Emulator& emulator, const ShellOptions& options);

}  // namespace bigquery_emulator_duckdb
