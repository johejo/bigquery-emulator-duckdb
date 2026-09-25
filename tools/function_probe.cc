// Probes every GoogleSQL built-in function signature against the emulator and prints a Markdown
// report of which ones translate and run. Each signature is called with sample arguments of its
// types, unless the hints file gives the calls for the function; a signature the probe cannot
// build a valid call for is reported as untested. A probe checks only that a call translates and
// runs on DuckDB, not that it returns what BigQuery would.

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
#include <variant>
#include <vector>

#include "absl/container/flat_hash_set.h"
#include "googlesql/public/builtin_function_options.h"
#include "googlesql/public/function.h"
#include "googlesql/public/function_signature.h"
#include "googlesql/public/simple_catalog.h"
#include "googlesql/public/type.h"
#include "googlesql/public/types/type_factory.h"
#include "src/analyzer.h"
#include "src/catalog.h"
#include "src/emulator.h"

namespace bigquery_emulator_duckdb {
namespace {

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
    // Date parts are the only enum arguments BigQuery spells as keywords.
    return type->DebugString().find("DateTimestampPart") != std::string::npos
               ? std::optional<std::string>("DAY")
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

// Calls for the functions whose generated samples do not make a valid call, and the functions
// that are left out of the report. See tools/function_probe_hints.txt for the format.
struct Hints {
  std::set<std::string> skip;
  std::map<std::string, std::vector<std::string>> queries;
};

Hints ReadHints(const std::string& path) {
  std::ifstream in(path);
  if (!in) {
    throw std::runtime_error("cannot read " + path);
  }
  Hints hints;
  std::string line;
  for (int number = 1; std::getline(in, line); ++number) {
    if (line.empty() || line.starts_with("#")) {
      continue;
    }
    std::istringstream words(line);
    std::string directive;
    std::string name;
    words >> directive >> name;
    std::string rest;
    std::getline(words >> std::ws, rest);
    if (directive == "skip" && !name.empty()) {
      hints.skip.insert(name);
    } else if (directive == "query" && !rest.empty()) {
      hints.queries[name].push_back(rest);
    } else {
      throw std::runtime_error(path + ":" + std::to_string(number) + ": cannot parse " + line);
    }
  }
  return hints;
}

std::string FirstLine(const std::string& text) { return text.substr(0, text.find('\n')); }

// The generated query for a signature, or why there is none.
std::variant<std::string, Probe> GeneratedQuery(const googlesql::Function& function,
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
  std::string call = function.SQLName() + "(";
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
  const std::vector<std::string> args(argv + 1, argv + argc);
  const bool list_signatures = std::ranges::find(args, "--signatures") != args.end();
  const auto hints_path = std::ranges::find_if(
      args, [](const std::string& arg) { return !arg.starts_with("--"); });
  if (hints_path == args.end()) {
    std::cerr << "usage: function_probe [--signatures] HINTS_FILE\n";
    return 2;
  }
  const Hints hints = ReadHints(*hints_path);

  googlesql::TypeFactory type_factory;
  googlesql::SimpleCatalog catalog("builtins", &type_factory);
  if (const absl::Status status = catalog.AddBuiltinFunctionsAndTypes(
          googlesql::BuiltinFunctionOptions(GoogleSqlLanguageOptions()));
      !status.ok()) {
    std::cerr << status << "\n";
    return 1;
  }
  absl::flat_hash_set<const googlesql::Function*> set;
  if (const absl::Status status = catalog.GetFunctions(&set); !status.ok()) {
    std::cerr << status << "\n";
    return 1;
  }
  std::vector<const googlesql::Function*> functions;
  std::set<std::string> names;
  for (const googlesql::Function* function : set) {
    // Operators and internal functions start with $; they are covered by the SQL tests.
    if (!function->Name().starts_with("$")) {
      functions.push_back(function);
      names.insert(function->SQLName());
    }
  }
  std::ranges::sort(functions, {}, [](const googlesql::Function* f) { return f->SQLName(); });
  // A hint for a function that does not exist is a typo or a function GoogleSQL dropped.
  for (const auto& name : hints.skip) {
    if (!names.contains(name)) {
      std::cerr << "hint names an unknown function: " << name << "\n";
      return 1;
    }
  }
  for (const auto& [name, queries] : hints.queries) {
    if (!names.contains(name)) {
      std::cerr << "hint names an unknown function: " << name << "\n";
      return 1;
    }
  }

  Prober prober;
  std::map<std::string, int> totals;
  if (!list_signatures) {
    std::cout << "| Function | Kind | Status | Notes |\n| --- | --- | --- | --- |\n";
  }
  for (const googlesql::Function* function : functions) {
    const std::string name = function->SQLName();
    if (hints.skip.contains(name)) {
      continue;
    }
    std::vector<Probe> probes;
    if (const auto hinted = hints.queries.find(name); hinted != hints.queries.end()) {
      for (const std::string& sql : hinted->second) {
        probes.push_back(prober.Run(sql));
      }
    } else {
      for (const googlesql::FunctionSignature& signature : function->signatures()) {
        if (signature.HideInSupportedSignatureList(GoogleSqlLanguageOptions())) {
          continue;
        }
        const auto query = GeneratedQuery(*function, signature);
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
    std::cout << "| `" << name << "` | "
              << (function->IsAnalytic()    ? "analytic"
                  : function->IsAggregate() ? "aggregate"
                                            : "scalar")
              << " | " << status << " | " << Escape(note) << " |\n";
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
