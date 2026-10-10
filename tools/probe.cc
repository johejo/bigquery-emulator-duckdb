#include "tools/probe.h"

#include <cstddef>
#include <exception>
#include <fstream>
#include <istream>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "googlesql/public/types/type_factory.h"
#include "src/analyzer.h"
#include "src/catalog.h"
#include "src/emulator.h"

namespace bigquery_emulator_duckdb {

std::vector<std::pair<std::string, std::string>> ReadLines(const std::string& path) {
  std::ifstream in(path);
  if (!in) {
    throw std::runtime_error("cannot read " + path);
  }
  std::vector<std::pair<std::string, std::string>> lines;
  std::string line;
  for (int number = 1; std::getline(in, line); ++number) {
    if (line.empty() || line.starts_with('#')) {
      continue;
    }
    std::istringstream words(line);
    std::string first;
    std::string rest;
    words >> first;
    std::getline(words >> std::ws, rest);
    if (rest.empty()) {
      std::string message = path;
      message += ":" + std::to_string(number) + ": cannot parse " + line;
      throw std::runtime_error(message);
    }
    lines.emplace_back(first, rest);
  }
  return lines;
}

std::string FirstLine(const std::string& text) { return text.substr(0, text.find('\n')); }

std::string Escape(const std::string& text) {
  std::string out;
  for (char const c : text) {
    if (c == '|') {
      out += "\\|";
    } else {
      out += c;
    }
  }
  return out;
}

std::string_view Status(std::map<Outcome, int> counts) {
  const int tested =
      counts[Outcome::kRuns] + counts[Outcome::kUnsupported] + counts[Outcome::kFailsOnDuckDb];
  if (counts[Outcome::kFailsOnDuckDb] > 0) {
    return "Broken";
  }
  if (tested == 0) {
    return "Untested";
  }
  if (counts[Outcome::kRuns] == tested) {
    return "Supported";
  }
  return counts[Outcome::kRuns] == 0 ? "Unsupported" : "Partial";
}

Probe RunProbe(Emulator& emulator, TableSource& tables, const std::string& sql) {
  // Analyze separately so that a query the probe got wrong is not blamed on the emulator.
  try {
    googlesql::TypeFactory type_factory;
    BigQueryCatalog catalog(tables, &type_factory, "test", "");
    AnalyzeGoogleSql(sql, catalog, type_factory, {.default_project = "test"});
  } catch (const std::exception& error) {
    // A feature whose language option the emulator leaves off, such as COLLATE, is one it does
    // not support rather than a query the probe got wrong.
    std::string const message = FirstLine(error.what());
    static const std::string kInvalid = "INVALID_ARGUMENT: ";
    if (const size_t end = message.find(" is not supported"); end != std::string::npos) {
      const size_t start = message.starts_with(kInvalid) ? kInvalid.size() : 0;
      return {.outcome = Outcome::kUnsupported, .detail = message.substr(start, end - start)};
    }
    return {.outcome = Outcome::kUntested, .detail = "`" + sql + "`: " + message};
  }

  QueryRequest request;
  request.project_id = "test";
  request.query = sql;
  const std::shared_ptr<const Job> job = emulator.RunQuery(request);
  if (!job->error.has_value()) {
    return {.outcome = Outcome::kRuns, .detail = ""};
  }
  std::string message = FirstLine(job->error->what());
  static const std::string kUnsupported = "The emulator does not support ";
  if (message.starts_with(kUnsupported)) {
    return {.outcome = Outcome::kUnsupported, .detail = message.substr(kUnsupported.size())};
  }
  // DuckDB's hint to add casts is noise in a report.
  if (const auto hint = message.find(". You might need"); hint != std::string::npos) {
    message.resize(hint);
  }
  return {.outcome = Outcome::kFailsOnDuckDb, .detail = "`" + sql + "`: " + message};
}

}  // namespace bigquery_emulator_duckdb
