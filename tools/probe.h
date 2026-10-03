// Pieces of //:function_probe, which generates docs/functions.md: reading its input files,
// running a query against the emulator and summarizing the outcomes as a status.

#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "src/catalog.h"
#include "src/emulator.h"

namespace bigquery_emulator_duckdb {

enum class Outcome : std::uint8_t { kRuns, kFailsOnDuckDb, kUnsupported, kUntested };

struct Probe {
  Outcome outcome;
  std::string detail;
};

// The lines of `path` that are neither blank nor comments, split into the first word and the rest.
std::vector<std::pair<std::string, std::string>> ReadLines(const std::string& path);

std::string FirstLine(const std::string& text);

// `text` with the pipes that would end a Markdown table cell escaped.
std::string Escape(const std::string& text);

// Supported, Partial, Unsupported, Broken or Untested, from how many probes had each outcome.
std::string Status(std::map<Outcome, int> counts);

// Runs `sql` on `emulator` in project `test` and reports whether it translates and runs. The query
// is analyzed first against `tables`, so a query that is not valid GoogleSQL is reported as
// untested rather than blamed on the emulator.
Probe RunProbe(Emulator& emulator, TableSource& tables, const std::string& sql);

}  // namespace bigquery_emulator_duckdb
