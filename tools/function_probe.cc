// Builds the calls that probe the BigQuery functions listed in tools/bigquery_functions.txt, and
// prints them for `restprobe functions`, which runs them on the emulator and writes
// docs/functions.md. Each signature the analyzer knows for a function is called with sample
// arguments of its types, unless the hints file gives the calls; a signature the probe cannot build
// a valid call for is reported as untested. The hints file gives the samples, the values that
// templated arguments take, and notes on a function's behavior, so that changing them needs no
// rebuild.
//
// Each output line is a JSON object: the function's name, the pages that document it, how the
// emulator implements it, its note, the queries to run and the probes settled without one.

#include <algorithm>
#include <cstddef>
#include <exception>
#include <fstream>
#include <iostream>
#include <istream>
#include <iterator>
#include <map>
#include <optional>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "absl/strings/ascii.h"
#include "absl/strings/str_split.h"
#include "googlesql/public/function.h"
#include "googlesql/public/function_signature.h"
#include "googlesql/public/type.h"
#include "googlesql/public/types/type_factory.h"
#include "nlohmann/json.hpp"
#include "src/catalog.h"
#include "src/field_schema.h"
#include "src/translator/functions.h"

namespace bigquery_emulator_duckdb {
namespace {

class NoTables : public TableSource {
 public:
  std::optional<std::vector<FieldSchema>> FindTable(const std::string& /*project*/,
                                                    const std::string& /*dataset*/,
                                                    const std::string& /*table*/) override {
    return std::nullopt;
  }
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

// A hint that applies to the signatures whose DebugString the pattern matches.
struct Selector {
  std::string pattern;
  std::regex regex;
  bool used = false;

  explicit Selector(const std::string& text) : pattern(text), regex(text) {}

  bool Matches(const std::string& signature) {
    const bool matches = std::regex_search(signature, regex);
    used = used || matches;
    return matches;
  }
};

struct Hints {
  // Calls that replace the generated ones, by function.
  std::map<std::string, std::vector<std::string>> queries;
  // Behavior a probe cannot observe, such as where results differ from DuckDB's defaults.
  std::map<std::string, std::string> notes;
  // The literal for an argument of a type, or of a templated kind such as <T1>.
  std::map<std::string, std::string> samples;
  // The literal for an optional argument in the longest form.
  std::map<std::string, std::string> optional_samples;
  std::map<std::string, std::vector<std::string>> values;
  // A set of values and, optionally, the concrete argument type that takes them.
  std::vector<std::pair<Selector, std::pair<std::string, std::string>>> elements;
  // The literal for arguments of a type in a signature.
  std::vector<std::pair<Selector, std::pair<std::string, std::string>>> arguments;
  // The signatures also probed in their longest form.
  std::vector<Selector> longest;

  // The values of the last `elements` hint that matches the signature, or one empty value.
  std::pair<std::vector<std::string>, std::string> Elements(const std::string& signature) {
    std::pair<std::vector<std::string>, std::string> result = {{""}, ""};
    for (auto& [selector, element] : elements) {
      if (selector.Matches(signature)) {
        result = {values.at(element.first), element.second};
      }
    }
    return result;
  }

  // The literal of the last `argument` hint for `type` that matches the signature.
  std::optional<std::string> Argument(const std::string& signature, const std::string& type) {
    std::optional<std::string> result;
    for (auto& [selector, argument] : arguments) {
      if (argument.first == type && selector.Matches(signature)) {
        result = argument.second;
      }
    }
    return result;
  }

  bool Longest(const std::string& signature) {
    bool result = false;
    for (Selector& selector : longest) {
      result = selector.Matches(signature) || result;
    }
    return result;
  }

  // The patterns that matched no signature, which are typos or hints for signatures that are gone.
  [[nodiscard]] std::vector<std::string> Unused() const {
    std::vector<std::string> result;
    for (const auto& [selector, set] : elements) {
      if (!selector.used) {
        result.push_back(selector.pattern);
      }
    }
    for (const auto& [selector, argument] : arguments) {
      if (!selector.used) {
        result.push_back(selector.pattern);
      }
    }
    for (const Selector& selector : longest) {
      if (!selector.used) {
        result.push_back(selector.pattern);
      }
    }
    return result;
  }
};

Hints ReadHints(const std::string& path,
                const std::map<std::string, std::vector<std::string>>& functions) {
  Hints hints;
  for (const auto& [directive, hint] : ReadLines(path)) {
    const std::vector<std::string> fields = absl::StrSplit(hint, " | ");
    const auto fail = [&](const std::string& why) {
      std::string message = path;
      message += ": " + why;
      message += ": " + directive;
      message += " " + hint;
      throw std::runtime_error(message);
    };
    if (directive == "query" || directive == "note") {
      const std::string name = hint.substr(0, hint.find(' '));
      const std::string text = hint.substr(std::min(hint.size(), name.size() + 1));
      if (text.empty() || (directive == "note" && hints.notes.contains(name))) {
        fail("cannot parse hint");
      }
      // A hint for a function BigQuery does not have is a typo or a function BigQuery dropped.
      if (!functions.contains(name)) {
        fail("names a function BigQuery does not have");
      }
      if (directive == "note") {
        hints.notes[name] = text;
      } else {
        hints.queries[name].push_back(text);
      }
    } else if ((directive == "sample" || directive == "optional") && fields.size() == 2) {
      (directive == "sample" ? hints.samples : hints.optional_samples)[fields.at(0)] = fields.at(1);
    } else if (directive == "values" && fields.size() >= 2) {
      hints.values[fields.at(0)] = {fields.begin() + 1, fields.end()};
    } else if (directive == "elements" && (fields.size() == 2 || fields.size() == 3)) {
      if (!hints.values.contains(fields.at(1))) {
        fail("names values not defined above");
      }
      hints.elements.emplace_back(Selector(fields.at(0)),
                                  std::pair(fields.at(1), fields.size() == 3 ? fields.at(2) : ""));
    } else if (directive == "argument" && fields.size() == 3) {
      hints.arguments.emplace_back(Selector(fields.at(0)), std::pair(fields.at(1), fields.at(2)));
    } else if (directive == "longest" && fields.size() == 1) {
      hints.longest.emplace_back(fields.at(0));
    } else {
      fail("cannot parse hint");
    }
  }
  return hints;
}

std::optional<std::string> Find(const std::map<std::string, std::string>& map,
                                const std::string& key) {
  const auto it = map.find(key);
  return it == map.end() ? std::nullopt : std::optional(it->second);
}

// A literal of `type`: its sample, or one built from the samples of its elements or fields.
std::optional<std::string> Sample(const googlesql::Type* type, const Hints& hints) {
  if (auto sample = Find(hints.samples, type->DebugString())) {
    return sample;
  }
  if (type->IsArray()) {
    const auto element = Sample(type->AsArray()->element_type(), hints);
    return element ? std::optional("[" + *element + "]") : std::nullopt;
  }
  if (type->IsStruct()) {
    std::string fields;
    for (const googlesql::StructField& field : type->AsStruct()->fields()) {
      const auto value = Sample(field.type, hints);
      if (!value) {
        return std::nullopt;
      }
      fields += (fields.empty() ? "" : ", ") + *value +
                (field.name.empty() ? "" : " AS `" + field.name + "`");
    }
    return "STRUCT(" + fields + ")";
  }
  return std::nullopt;
}

// The argument's type as signatures print it, without its cardinality and name: a type such as
// INT64 or a templated kind such as <T1>. Hints name types this way.
std::string TypeKey(const googlesql::FunctionArgumentType& argument) {
  if (argument.type() != nullptr) {
    return argument.type()->DebugString();
  }
  return argument.kind() == googlesql::ARG_KIND_EXPR_ARBITRARY
             ? "ANY TYPE"
             : googlesql::FunctionArgumentType::SignatureArgumentKindToString(argument.kind());
}

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
  return supported(signature.result_type()) &&
         std::ranges::all_of(signature.arguments(), supported);
}

// Whether the signature takes or returns a RANGE of its templated type.
bool IsRangeSignature(const googlesql::FunctionSignature& signature) {
  return signature.result_type().kind() == googlesql::ARG_KIND_EXPR_RANGE_ANY_1 ||
         std::ranges::any_of(signature.arguments(), [](const auto& argument) {
           return argument.kind() == googlesql::ARG_KIND_EXPR_RANGE_ANY_1;
         });
}

// A query to run, or why the probe could not build one.
struct Call {
  std::string sql;
  std::string untested;
};

// The call of a signature with <T1>, or `element_type`, bound to `element`.
Call GeneratedCall(const std::string& name, const googlesql::Function& function,
                   const googlesql::FunctionSignature& signature, const std::string& text,
                   Hints& hints, const std::string& element, const std::string& element_type,
                   bool longest) {
  // A RANGE of `element` starts at it, and is unbounded where a second element would end it.
  const bool range = IsRangeSignature(signature);
  bool element_used = false;
  std::vector<std::string> arguments;
  const googlesql::Type* previous_type = nullptr;
  for (const googlesql::FunctionArgumentType& argument : signature.arguments()) {
    if (argument.optional() && !argument.has_argument_name() && !longest) {
      break;  // Probe the shortest form.
    }
    if (argument.optional() && argument.has_argument_name()) {
      continue;
    }
    const std::string type = TypeKey(argument);
    const googlesql::SignatureArgumentKind kind = argument.kind();
    std::optional<std::string> sample;
    if (longest && argument.optional()) {
      sample = Find(hints.optional_samples, type);
    }
    if (!sample) {
      sample = hints.Argument(text, type);
    }
    if (!sample && !element.empty()) {
      if (!element_type.empty()) {
        if (type == element_type) {
          sample = element;
        }
      } else if (kind == googlesql::ARG_KIND_EXPR_RANGE_ANY_1) {
        sample = "RANGE(" + element + ", NULL)";
      } else if (kind == googlesql::ARG_KIND_EXPR_ANY_1) {
        sample = range && element_used ? "NULL" : element;
      } else if (kind == googlesql::ARG_KIND_EXPR_ARRAY_ANY_1) {
        sample = "[" + element + "]";
      } else if (kind == googlesql::ARG_KIND_EXPR_ARBITRARY) {
        sample = element;
      }
    }
    if (!sample) {
      sample =
          argument.type() != nullptr ? Sample(argument.type(), hints) : Find(hints.samples, type);
    }
    if (!sample) {
      return {.sql = "", .untested = "no sample for " + argument.DebugString()};
    }
    element_used = element_used || kind == googlesql::ARG_KIND_EXPR_ANY_1;
    // Optional INT64/date-part pairs spell one INTERVAL expression, such as
    // GENERATE_DATE_ARRAY's step, rather than two comma-separated arguments.
    if (longest && argument.optional() && argument.type() != nullptr && argument.type()->IsEnum() &&
        previous_type != nullptr && previous_type->IsInt64()) {
      arguments.back() = "INTERVAL " + arguments.back() + " " + *sample;
    } else {
      arguments.push_back(*sample);
    }
    previous_type = argument.type();
  }
  std::string call = name + "(";
  for (size_t i = 0; i < arguments.size(); ++i) {
    call += (i == 0 ? "" : ", ") + arguments.at(i);
  }
  call += ')';
  return {
      .sql = function.IsAnalytic()
                 ? "SELECT " + call + " OVER (ORDER BY x) FROM UNNEST([1, 2]) AS x"
             : function.IsAggregate() ? "SELECT " + call + " FROM UNNEST([1, 2]) AS x"
                                      : "SELECT " + call,
      .untested = "",
  };
}

// The calls of each signature BigQuery has: one for each element value, and the longest form
// where the hints ask for it.
std::vector<std::pair<std::string, Call>> GeneratedCalls(const std::string& name,
                                                         const googlesql::Function& function,
                                                         Hints& hints) {
  std::vector<std::pair<std::string, Call>> calls;
  for (const googlesql::FunctionSignature& signature : function.signatures()) {
    if (!IsBigQuerySignature(signature)) {
      continue;
    }
    const std::string text = signature.DebugString(name);
    const auto [elements, element_type] = hints.Elements(text);
    std::ranges::transform(elements, std::back_inserter(calls), [&](const std::string& element) {
      return std::pair(text,
                       GeneratedCall(name, function, signature, text, hints, element, element_type,
                                     /*longest=*/false));
    });
    const bool optional = std::ranges::any_of(signature.arguments(), [](const auto& argument) {
      return argument.optional() && !argument.has_argument_name();
    });
    if (optional && hints.Longest(text)) {
      calls.emplace_back(text, GeneratedCall(name, function, signature, text, hints, "", "",
                                             /*longest=*/true));
    }
  }
  return calls;
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
  for (const auto& [name, doc] : ReadLines(args.at(0))) {
    functions[name].push_back(doc);
  }
  Hints hints = ReadHints(args.at(1), functions);

  googlesql::TypeFactory type_factory;
  NoTables tables;
  BigQueryCatalog catalog(tables, &type_factory, "test", "");
  for (const auto& [name, docs] : functions) {
    const googlesql::Function* function = nullptr;
    // Aliases such as CEILING resolve to the function they stand for.
    const std::vector<std::string> path = absl::StrSplit(name, '.');
    const bool known = catalog.FindFunction(path, &function).ok();
    // Functions spelled as syntax, such as EXTRACT, are operators such as $EXTRACT.
    const translator::FunctionEntry* entry =
        translator::FindFunction(known ? absl::AsciiStrToUpper(function->Name()) : "$" + name);
    const googlesql::TableValuedFunction* table_function = nullptr;
    // RANGE_SESSIONIZE and EXTERNAL_QUERY have scan translators rather than scalar registry
    // entries.
    const bool known_table_function = (name == "RANGE_SESSIONIZE" || name == "EXTERNAL_QUERY") &&
                                      catalog.FindTableValuedFunction(path, &table_function).ok();
    const std::string implementation =
        known_table_function ? "Table function"
        : entry == nullptr   ? ""
                             : std::string(translator::Describe(entry->implementation));
    nlohmann::json line = {
        {"name", name},
        {"docs", docs},
        {"implementation", implementation},
        {"note", hints.notes.contains(name) ? hints.notes.at(name) : ""},
        {"queries", nlohmann::json::array()},
        {"untested", nlohmann::json::array()},
        {"unsupported", nlohmann::json::array()},
    };
    if (const auto hinted = hints.queries.find(name); hinted != hints.queries.end()) {
      line["queries"] = hinted->second;
    } else if (known) {
      for (const auto& [signature, call] : GeneratedCalls(name, *function, hints)) {
        if (list_signatures) {
          std::cout << signature << "\n  " << (call.sql.empty() ? call.untested : call.sql) << "\n";
        } else if (call.sql.empty()) {
          line["untested"].push_back(call.untested);
        } else {
          line["queries"].push_back(call.sql);
        }
      }
    } else {
      line["unsupported"].push_back("the analyzer does not know this function");
    }
    if (!list_signatures) {
      std::cout << line.dump() << "\n";
    }
  }
  if (const std::vector<std::string> unused = hints.Unused(); !unused.empty()) {
    for (const std::string& pattern : unused) {
      std::cerr << args.at(1) << ": no signature matches " << pattern << "\n";
    }
    return 1;
  }
  return 0;
}

}  // namespace
}  // namespace bigquery_emulator_duckdb

int main(int argc, char** argv) {
  try {
    return bigquery_emulator_duckdb::Main(argc, argv);
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  } catch (...) {
    std::cerr << "Unknown error\n";
    return 1;
  }
}
