// Probes the BigQuery functions listed in tools/bigquery_functions.txt against the emulator and
// prints a Markdown report of which ones translate and run. Each signature the analyzer knows for a
// function is called with sample arguments of its types, unless the hints file gives the calls; a
// signature the probe cannot build a valid call for is reported as untested. A probe checks only
// that a call translates and runs on DuckDB, not that it returns what BigQuery would.

#include <algorithm>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "absl/strings/ascii.h"
#include "absl/strings/str_join.h"
#include "absl/strings/str_split.h"
#include "googlesql/public/function.h"
#include "googlesql/public/function_signature.h"
#include "googlesql/public/type.h"
#include "googlesql/public/types/type_factory.h"
#include "src/analyzer.h"
#include "src/catalog.h"
#include "src/emulator.h"

namespace bigquery_emulator_duckdb {
namespace {

constexpr std::string_view kDocs =
    "https://cloud.google.com/bigquery/docs/reference/standard-sql";

class NoTables : public TableSource {
 public:
  std::optional<std::vector<FieldSchema>> FindTable(const std::string&, const std::string&,
                                                    const std::string&) override {
    return std::nullopt;
  }
};

// A literal of `type`, or nothing for a type BigQuery does not have.
std::optional<std::string> Sample(const googlesql::Type* type) {
  if (type->IsArray()) {
    const auto element = Sample(type->AsArray()->element_type());
    return element ? std::optional("[" + *element + "]") : std::nullopt;
  }
  if (type->IsStruct()) {
    std::string fields;
    for (const googlesql::StructField& field : type->AsStruct()->fields()) {
      const auto value = Sample(field.type);
      if (!value) {
        return std::nullopt;
      }
      fields += (fields.empty() ? "" : ", ") + *value +
                (field.name.empty() ? "" : " AS `" + field.name + "`");
    }
    return "STRUCT(" + fields + ")";
  }
  if (type->IsEnum()) {
    // Date parts are the only enum arguments BigQuery spells as keywords; it spells rounding
    // modes as strings.
    const std::string name = type->DebugString();
    if (name.find("DateTimestampPart") != std::string::npos) {
      return "DAY";
    }
    return name.find("ROUNDING_MODE") != std::string::npos
               ? std::optional<std::string>("'ROUND_HALF_EVEN'")
               : std::nullopt;
  }
  if (type->IsRange()) {
    const auto element = type->AsRange()->element_type();
    return "RANGE<" + element->TypeName(googlesql::PRODUCT_EXTERNAL) + "> '[" +
           (element->IsDate() ? "2024-01-01, 2024-02-01" : "2024-01-01 00:00:00, 2024-02-01 00:00:00") +
           ")'";
  }
  switch (type->kind()) {
    case googlesql::TYPE_INT64:
      return "2";
    case googlesql::TYPE_DOUBLE:
      return "1.5";
    case googlesql::TYPE_NUMERIC:
      return "NUMERIC '1.5'";
    case googlesql::TYPE_BIGNUMERIC:
      return "BIGNUMERIC '1.5'";
    case googlesql::TYPE_BOOL:
      return "TRUE";
    case googlesql::TYPE_STRING:
      return "'abc'";
    case googlesql::TYPE_BYTES:
      return "b'abc'";
    case googlesql::TYPE_DATE:
      return "DATE '2024-01-15'";
    case googlesql::TYPE_DATETIME:
      return "DATETIME '2024-01-15 10:20:30'";
    case googlesql::TYPE_TIME:
      return "TIME '10:20:30'";
    case googlesql::TYPE_TIMESTAMP:
      return "TIMESTAMP '2024-01-15 10:20:30+00'";
    case googlesql::TYPE_INTERVAL:
      return "INTERVAL 1 DAY";
    case googlesql::TYPE_JSON:
      return "JSON '{\"a\": [1, 2]}'";
    case googlesql::TYPE_GEOGRAPHY:
      return "ST_GEOGPOINT(1, 2)";
    default:
      return std::nullopt;
  }
}

// A sample for an argument, with templated kinds bound to INT64 (and the second template to
// STRING, so that T1 and T2 can differ).
std::optional<std::string> Sample(const googlesql::FunctionArgumentType& argument) {
  if (argument.IsConcrete() || argument.kind() == googlesql::ARG_KIND_EXPR_FIXED) {
    return argument.type() ? Sample(argument.type()) : std::nullopt;
  }
  switch (argument.kind()) {
    case googlesql::ARG_KIND_EXPR_ANY_1:
    case googlesql::ARG_KIND_EXPR_ARBITRARY:
      return "2";
    case googlesql::ARG_KIND_EXPR_ANY_2:
    case googlesql::ARG_KIND_EXPR_STRING_ANY:
      return "'abc'";
    case googlesql::ARG_KIND_EXPR_ARRAY_ANY_1:
      return "[1, 2]";
    case googlesql::ARG_KIND_EXPR_ARRAY_ANY_2:
      return "['a', 'b']";
    case googlesql::ARG_KIND_EXPR_STRUCT_ANY:
      return "STRUCT(1 AS a)";
    case googlesql::ARG_KIND_EXPR_RANGE_ANY_1:
      return "RANGE<DATE> '[2024-01-01, 2024-02-01)'";
    default:
      return std::nullopt;
  }
}

enum class Outcome { kRuns, kFailsOnDuckDb, kUnsupported, kUntested };

struct Probe {
  Outcome outcome;
  std::string detail;
};

// The lines of `path` that are neither blank nor comments, split into the first word and the rest.
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
      throw std::runtime_error(path + ":" + std::to_string(number) + ": cannot parse " + line);
    }
    lines.emplace_back(first, rest);
  }
  return lines;
}

std::string FirstLine(const std::string& text) { return text.substr(0, text.find('\n')); }

// Whether BigQuery has the signature: GoogleSQL has signatures over types BigQuery lacks, such as
// FLOAT for CEILING.
bool IsBigQuerySignature(const googlesql::FunctionSignature& signature) {
  const googlesql::LanguageOptions& options = GoogleSqlLanguageOptions();
  if (signature.HideInSupportedSignatureList(options)) {
    return false;
  }
  const auto supported = [&](const googlesql::FunctionArgumentType& argument) {
    const googlesql::Type* type = argument.type();
    if (type != nullptr && type->IsArray()) {
      type = type->AsArray()->element_type();
    }
    return type == nullptr || (type->IsSupportedType(options) && !type->IsFloat());
  };
  return supported(signature.result_type()) && std::ranges::all_of(signature.arguments(), supported);
}

// The generated query for a signature, or why there is none.
std::variant<std::string, Probe> GeneratedQuery(const std::string& name,
                                                const googlesql::Function& function,
                                                const googlesql::FunctionSignature& signature) {
  std::vector<std::string> arguments;
  for (const googlesql::FunctionArgumentType& argument : signature.arguments()) {
    if (argument.optional() && !argument.has_argument_name()) {
      break;  // Probe the shortest form.
    }
    if (argument.optional()) {
      continue;
    }
    const auto sample = Sample(argument);
    if (!sample) {
      return Probe{Outcome::kUntested, "no sample for " + argument.DebugString()};
    }
    arguments.push_back(*sample);
  }
  std::string call = name + "(";
  for (size_t i = 0; i < arguments.size(); ++i) {
    call += (i == 0 ? "" : ", ") + arguments[i];
  }
  call += ")";
  return function.IsAnalytic()   ? "SELECT " + call + " OVER (ORDER BY x) FROM UNNEST([1, 2]) AS x"
         : function.IsAggregate() ? "SELECT " + call + " FROM UNNEST([1, 2]) AS x"
                                  : "SELECT " + call;
}

class Prober {
 public:
  Probe Run(const std::string& sql) {
    // Analyze separately so that a call the probe got wrong is not blamed on the emulator.
    try {
      googlesql::TypeFactory type_factory;
      NoTables tables;
      BigQueryCatalog catalog(tables, &type_factory, "test", "");
      AnalyzeGoogleSql(sql, catalog, type_factory);
    } catch (const std::exception& error) {
      return {Outcome::kUntested, "`" + sql + "`: " + FirstLine(error.what())};
    }

    QueryRequest request;
    request.project_id = "test";
    request.query = sql;
    const std::shared_ptr<const Job> job = emulator_.RunQuery(request);
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

 private:
  Emulator emulator_;
};

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

// The page that documents the function itself rather than a variant of it, such as AVG over
// aggregate-dp-functions#dp_avg: the one whose anchor is the function's name.
std::string MainDoc(const std::string& name, const std::vector<std::string>& docs) {
  std::string anchor = absl::AsciiStrToLower(name);
  std::erase(anchor, '.');
  for (const std::string& doc : docs) {
    if (doc.ends_with("#" + anchor)) {
      return doc;
    }
  }
  return docs.front();
}

// The pages that document the function, which name the categories it belongs to.
std::string Categories(const std::vector<std::string>& docs) {
  std::set<std::string> pages;
  for (const std::string& doc : docs) {
    pages.insert(doc.substr(0, doc.find('#')));
  }
  return absl::StrJoin(pages, ", ");
}

std::string Status(std::map<Outcome, int> counts) {
  const int tested = counts[Outcome::kRuns] + counts[Outcome::kUnsupported] +
                     counts[Outcome::kFailsOnDuckDb];
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

int Main(int argc, char** argv) {
  std::vector<std::string> args(argv + 1, argv + argc);
  const bool list_signatures = std::erase(args, "--signatures") > 0;
  if (args.size() != 2) {
    std::cerr << "usage: function_probe [--signatures] FUNCTIONS_FILE HINTS_FILE\n";
    return 2;
  }
  // The pages and anchors that document each function. A name can be documented on several
  // pages, such as EXTRACT for each type it takes, and the probe covers them in one row.
  std::map<std::string, std::vector<std::string>> functions;
  for (const auto& [name, doc] : ReadLines(args[0])) {
    functions[name].push_back(doc);
  }
  std::map<std::string, std::vector<std::string>> hints;
  for (const auto& [directive, hint] : ReadLines(args[1])) {
    const std::string name = hint.substr(0, hint.find(' '));
    const std::string sql = hint.substr(std::min(hint.size(), name.size() + 1));
    if (directive != "query" || sql.empty()) {
      std::cerr << "cannot parse hint: " << directive << " " << hint << "\n";
      return 1;
    }
    // A hint for a function BigQuery does not have is a typo or a function BigQuery dropped.
    if (!functions.contains(name)) {
      std::cerr << "hint names a function not in " << args[0] << ": " << name << "\n";
      return 1;
    }
    hints[name].push_back(sql);
  }

  googlesql::TypeFactory type_factory;
  NoTables tables;
  BigQueryCatalog catalog(tables, &type_factory, "test", "");
  Prober prober;
  std::map<std::string, int> totals;
  if (!list_signatures) {
    std::cout << "| Function | Category | Status | Notes |\n| --- | --- | --- | --- |\n";
  }
  for (const auto& [name, docs] : functions) {
    std::vector<Probe> probes;
    const googlesql::Function* function = nullptr;
    // Aliases such as CEILING resolve to the function they stand for.
    const std::vector<std::string> path = absl::StrSplit(name, '.');
    if (const auto hinted = hints.find(name); hinted != hints.end()) {
      for (const std::string& sql : hinted->second) {
        probes.push_back(prober.Run(sql));
      }
    } else if (catalog.FindFunction(path, &function).ok()) {
      for (const googlesql::FunctionSignature& signature : function->signatures()) {
        if (!IsBigQuerySignature(signature)) {
          continue;
        }
        const auto query = GeneratedQuery(name, *function, signature);
        if (list_signatures) {
          std::cout << signature.DebugString(name) << "\n  "
                    << (std::holds_alternative<std::string>(query)
                            ? std::get<std::string>(query)
                            : std::get<Probe>(query).detail)
                    << "\n";
          continue;
        }
        probes.push_back(std::holds_alternative<std::string>(query)
                             ? prober.Run(std::get<std::string>(query))
                             : std::get<Probe>(query));
      }
    } else {
      probes.push_back({Outcome::kUnsupported, "the analyzer does not know this function"});
    }
    if (list_signatures || probes.empty()) {
      continue;
    }
    std::map<Outcome, int> counts;
    std::set<std::string> notes;
    for (const Probe& probe : probes) {
      ++counts[probe.outcome];
      if (probe.outcome != Outcome::kRuns) {
        notes.insert(probe.detail);
      }
    }
    const std::string status = Status(counts);
    ++totals[status];
    std::string note;
    for (const std::string& n : notes) {
      note += (note.empty() ? "" : "; ") + n;
    }
    std::cout << "| [`" << name << "`](" << kDocs << "/" << MainDoc(name, docs) << ") | "
              << Categories(docs) << " | " << status << " | " << Escape(note) << " |\n";
  }
  for (const auto& [status, count] : totals) {
    std::cerr << status << "=" << count << " ";
  }
  std::cerr << "\n";
  return 0;
}

}  // namespace
}  // namespace bigquery_emulator_duckdb

int main(int argc, char** argv) { return bigquery_emulator_duckdb::Main(argc, argv); }
