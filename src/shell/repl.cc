#include "src/shell/repl.h"

#include <csignal>
#include <string>
#include <string_view>

#include "src/shell/line_reader.h"

namespace bigquery_emulator_duckdb::shell {

namespace {

std::string_view Trim(std::string_view text) {
  const auto begin = text.find_first_not_of(" \t\r\n");
  if (begin == std::string_view::npos) {
    return {};
  }
  return text.substr(begin, text.find_last_not_of(" \t\r\n") - begin + 1);
}

}  // namespace

void Interact(const ReplHooks& hooks) {
  // Ctrl+C while a job runs only discards what was typed meanwhile, which the terminal does.
  static_cast<void>(std::signal(SIGINT, SIG_IGN));
  std::string entry;
  std::string line;
  while (true) {
    const std::string prompt = hooks.prompt();
    const LineStatus status =
        ReadLine(entry.empty() ? prompt + "> " : std::string(prompt.size(), ' ') + "-> ", line);
    if (status == LineStatus::kInterrupted) {
      entry.clear();
      continue;
    }
    if (status == LineStatus::kEnd) {
      return;
    }
    if (entry.empty()) {
      const std::string_view command = Trim(line);
      if (command.empty()) {
        continue;
      }
      if (command.front() == '.' || command.front() == '\\') {
        if (!hooks.command(command.substr(1))) {
          return;
        }
        continue;
      }
    }
    entry += line;
    entry += '\n';
    if (hooks.is_complete(entry)) {
      hooks.run(entry);
      entry.clear();
    }
  }
}

}  // namespace bigquery_emulator_duckdb::shell
