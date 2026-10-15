#pragma once

#include <functional>
#include <string>
#include <string_view>

namespace bigquery_emulator_duckdb::shell {

// What the loop asks of the code that runs entries; it knows nothing of queries itself.
struct ReplHooks {
  // Precedes "> " in the prompt, such as the default project and dataset.
  std::function<std::string()> prompt;
  // Whether `entry`, its lines each ending with a newline, is ready to run.
  std::function<bool(std::string_view entry)> is_complete;
  std::function<void(const std::string& entry)> run;
  // Runs a line that starts with '.' or '\' outside an entry, trimmed and without its prefix;
  // returns false to quit.
  std::function<bool(std::string_view command)> command;
};

// Reads entries from the terminal at standard input until the end of input or a command quits.
// Ctrl+C discards the entry being typed.
void Interact(const ReplHooks& hooks);

}  // namespace bigquery_emulator_duckdb::shell
