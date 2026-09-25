#include "tools/probe.h"

#include <exception>
#include <fstream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "googlesql/public/types/type_factory.h"
#include "src/analyzer.h"

namespace bigquery_emulator_duckdb {

std::vector<std::pair<std::string, std::string>> ReadLines(const std::string& path) {
  std::ifstream in(path);
  if (!in) {
    throw std::runtime_error("cannot read " + path);
  }
  std::vector<std::pair<std::string, std::string>> lines;
  std::string line;
  for (int number = 1; std::getline(in, line); ++number) {
    if (line.empty() || line.starts_with("#")) {
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
  for (char c : text) {
    if (c == '|') {
      out += "\\|";
    } else {
      out += c;
    }
  }
  return out;
}

std::string Status(std::map<Outcome, int> counts) {
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

Probe RunProbe(Emulator& emulator, TableSource& tables, const std::string& sql,
               const std::string& default_dataset) {
  // Analyze separately so that a query the probe got wrong is not blamed on the emulator.
  try {
    googlesql::TypeFactory type_factory;
    BigQueryCatalog catalog(tables, &type_factory, "test", default_dataset);
    AnalyzeGoogleSql(sql, catalog, type_factory,
                     {.default_project = "test", .default_dataset = default_dataset});
  } catch (const std::exception& error) {
    // A feature whose language option the emulator leaves off, such as COLLATE, is one it does
    // not support rather than a query the probe got wrong.
    std::string message = FirstLine(error.what());
    static const std::string kInvalid = "INVALID_ARGUMENT: ";
    if (const size_t end = message.find(" is not supported"); end != std::string::npos) {
      const size_t start = message.starts_with(kInvalid) ? kInvalid.size() : 0;
      return {Outcome::kUnsupported, message.substr(start, end - start)};
    }
    return {Outcome::kUntested, "`" + sql + "`: " + message};
  }

  QueryRequest request;
  request.project_id = "test";
  request.query = sql;
  if (!default_dataset.empty()) {
    request.default_dataset = DatasetReference{"test", default_dataset};
  }
  const std::shared_ptr<const Job> job = emulator.RunQuery(request);
  if (!job->error.has_value()) {
    return {Outcome::kRuns, ""};
  }
  std::string message = FirstLine(job->error->what());
  static const std::string kUnsupported = "The emulator does not support ";
  if (message.starts_with(kUnsupported)) {
    return {Outcome::kUnsupported, message.substr(kUnsupported.size())};
  }
  // DuckDB's hint to add casts is noise in a report.
  message = message.substr(0, message.find(". You might need"));
  return {Outcome::kFailsOnDuckDb, "`" + sql + "`: " + message};
}

}  // namespace bigquery_emulator_duckdb
