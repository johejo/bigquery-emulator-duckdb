#include "src/shell/line_reader.h"

#include <unistd.h>

#include <cerrno>
#include <string>
#include <vector>

#include "linenoise.h"

namespace bigquery_emulator_duckdb::shell {

// The terminal is in raw mode only while the line is edited, so Ctrl+C arrives as a byte rather
// than as SIGINT.
LineStatus ReadLine(const std::string& prompt, std::string& line) {
  // linenoise drops what does not fit, so leave room for long pasted lines.
  std::vector<char> buffer(1U << 20U);
  linenoiseState state{};
  if (linenoiseEditStart(&state, STDIN_FILENO, STDERR_FILENO, buffer.data(), buffer.size(),
                         prompt.c_str()) != 0) {
    return LineStatus::kEnd;
  }
  char* result = linenoiseEditMore;
  while (result == linenoiseEditMore) {
    result = linenoiseEditFeed(&state);
  }
  linenoiseEditStop(&state);
  if (result == nullptr) {
    return errno == EAGAIN ? LineStatus::kInterrupted : LineStatus::kEnd;
  }
  line = result;
  linenoiseFree(result);
  if (line.find_first_not_of(" \t\r\n") != std::string::npos) {
    linenoiseHistoryAdd(line.c_str());
  }
  return LineStatus::kLine;
}

}  // namespace bigquery_emulator_duckdb::shell
