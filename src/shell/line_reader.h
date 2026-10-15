#pragma once

#include <cstdint>
#include <string>

namespace bigquery_emulator_duckdb::shell {

enum class LineStatus : std::uint8_t { kLine, kEnd, kInterrupted };

// Reads a line from the terminal at standard input into `line`, writing `prompt` and the editing
// to standard error, since standard output carries rows. Returns kInterrupted on Ctrl+C and kEnd
// on Ctrl+D at an empty line or an error. Non-blank lines join the session's history.
LineStatus ReadLine(const std::string& prompt, std::string& line);

}  // namespace bigquery_emulator_duckdb::shell
