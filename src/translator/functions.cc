#include "src/translator/functions.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <iterator>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "src/translator/functions_aggregate.h"
#include "src/translator/functions_backend.h"
#include "src/translator/functions_template.h"
#include "src/translator/handlers.h"

namespace bigquery_emulator_duckdb::translator {
namespace {

struct NamedFunction {
  std::string_view target;
  Implementation implementation = Implementation::kRenamed;
};

// Functions called by another name, with the same arguments.
constexpr auto kFunctionNames = std::to_array<std::pair<std::string_view, NamedFunction>>({
    {"GENERATE_UUID", {.target = "uuid"}},
    {"IS_INF", {.target = "isinf"}},
    {"IS_NAN", {.target = "isnan"}},
    {"RAND", {.target = "random"}},
    {"SESSION_USER", {.target = "bq_session_user", .implementation = Implementation::kCustom}},
    {"UNIX_MICROS", {.target = "epoch_us"}},
});

// BigQuery functions that DuckDB has under the same name, with the same arguments. Passing
// arbitrary builtin names through would accidentally accept internal functions and overloads
// with different semantics. Extend this list with execution coverage.
constexpr auto kPlainFunctions = std::to_array<std::string_view>({
    "IFNULL",
    "COALESCE",
    "CHAR_LENGTH",
    "CHARACTER_LENGTH",
    "ARRAY_LENGTH",
});

struct HandledFunction {
  Handler handler;
  Implementation implementation = Implementation::kSQLInCode;
};

// Functions that src/translator/handlers.cc translates in code.
constexpr auto kHandlers = std::to_array<std::pair<std::string_view, HandledFunction>>({
    {"$MAKE_ARRAY", {.handler = MakeArray}},
    {"$AND", {.handler = Logical}},
    {"$OR", {.handler = Logical}},
    {"$IN", {.handler = InList}},
    {"$CASE_NO_VALUE", {.handler = Case}},
    {"$CASE_WITH_VALUE", {.handler = Case}},
    {"SAME", {.handler = GraphIdentityPredicate}},
    {"ALL_DIFFERENT", {.handler = GraphIdentityPredicate}},
    {"DATE_BUCKET", {.handler = Bucket}},
    {"DATETIME_BUCKET", {.handler = Bucket}},
    {"TIMESTAMP_BUCKET", {.handler = Bucket}},
    {"TO_JSON", {.handler = ToJson, .implementation = Implementation::kGoogleSQL}},
    {"TO_JSON_STRING", {.handler = ToJson, .implementation = Implementation::kGoogleSQL}},
    {"JSON_ARRAY", {.handler = JsonArray, .implementation = Implementation::kGoogleSQL}},
    {"JSON_REMOVE", {.handler = JsonRemove, .implementation = Implementation::kGoogleSQL}},
    {"JSON_SET", {.handler = JsonSet, .implementation = Implementation::kGoogleSQL}},
    {
        "JSON_ARRAY_APPEND",
        {.handler = JsonArrayModify, .implementation = Implementation::kGoogleSQL},
    },
    {
        "JSON_ARRAY_INSERT",
        {.handler = JsonArrayModify, .implementation = Implementation::kGoogleSQL},
    },
    {"JSON_OBJECT", {.handler = JsonObject, .implementation = Implementation::kGoogleSQL}},
    {"FORMAT", {.handler = Format, .implementation = Implementation::kGoogleSQL}},
    {"ARRAY_CONCAT", {.handler = ArrayConcat}},
    {"CONCAT", {.handler = ConcatStrings}},
    {"GREATEST", {.handler = Extremum}},
    {"LEAST", {.handler = Extremum}},
    {
        "KEYS.ADD_KEY_FROM_RAW_BYTES",
        {.handler = AddKeyFromRawBytes, .implementation = Implementation::kGoogleSQL},
    },
    {"TEXT_ANALYZE", {.handler = TextAnalyze, .implementation = Implementation::kGoogleSQL}},
    {"OBJ.MAKE_REF", {.handler = MakeRef, .implementation = Implementation::kCustom}},
    {"OBJ.FETCH_METADATA", {.handler = FetchMetadata, .implementation = Implementation::kCustom}},
});

// SAFE_ADD and its siblings are the arithmetic operators with the SAFE. prefix.
constexpr auto kSafeFunctions = std::to_array<std::pair<std::string_view, std::string_view>>({
    {"SAFE_ADD", "$ADD"},
    {"SAFE_SUBTRACT", "$SUBTRACT"},
    {"SAFE_MULTIPLY", "$MULTIPLY"},
    {"SAFE_NEGATE", "$UNARY_MINUS"},
});

// The functions that handle BIGNUMERIC, by carrying it as it is, by comparing BIGNUM, which
// DuckDB does exactly, or by rules of their own.
constexpr auto kBigNumericFunctions = [] {
  auto functions = std::to_array<std::string_view>({
      // Comparisons.
      "$EQUAL",
      "$NOT_EQUAL",
      "$LESS",
      "$LESS_OR_EQUAL",
      "$GREATER",
      "$GREATER_OR_EQUAL",
      "$BETWEEN",
      "$IS_DISTINCT_FROM",
      "$IS_NOT_DISTINCT_FROM",
      "$IS_NULL",
      "$IN",
      "$IN_ARRAY",
      "GREATEST",
      "LEAST",
      "RANGE_BUCKET",
      // Carrying values.
      "$CASE_NO_VALUE",
      "$CASE_WITH_VALUE",
      "IF",
      "IFNULL",
      "COALESCE",
      "NULLIF",
      // ERROR returns no value; GoogleSQL calls it with the BIGNUMERIC type of ARRAY_FIRST and
      // ARRAY_LAST.
      "ERROR",
      "$MAKE_ARRAY",
      "$ARRAY_AT_OFFSET",
      "$ARRAY_AT_ORDINAL",
      "$SAFE_ARRAY_AT_OFFSET",
      "$SAFE_ARRAY_AT_ORDINAL",
      // Arithmetic.
      "$ADD",
      "$SUBTRACT",
      "$UNARY_MINUS",
      "$MULTIPLY",
      "$DIVIDE",
      "SAFE_DIVIDE",
      "DIV",
      "MOD",
      "ABS",
      "SIGN",
      "ROUND",
      "TRUNC",
      "CEIL",
      "CEILING",
      "FLOOR",
      "SQRT",
      "CBRT",
      "POW",
      "POWER",
      "EXP",
      "LN",
      "LOG",
      "LOG10",
      // Arrays, JSON and text.
      "ARRAY_LENGTH",
      "ARRAY_REVERSE",
      "ARRAY_CONCAT",
      "GENERATE_ARRAY",
      "TO_JSON",
      "TO_JSON_STRING",
      "JSON_ARRAY",
      "JSON_OBJECT",
      "JSON_SET",
      "JSON_ARRAY_APPEND",
      "JSON_ARRAY_INSERT",
      "FORMAT",
      "PARSE_BIGNUMERIC",
      // Aggregate and analytic functions.
      "COUNT",
      "MIN",
      "MAX",
      "ANY_VALUE",
      "ARRAY_AGG",
      "ARRAY_CONCAT_AGG",
      "MAX_BY",
      "MIN_BY",
      "APPROX_COUNT_DISTINCT",
      "HLL_COUNT.INIT",
      "APPROX_QUANTILES",
      "APPROX_TOP_COUNT",
      "APPROX_TOP_SUM",
      "SUM",
      "AVG",
      "STDDEV",
      "STDDEV_SAMP",
      "STDDEV_POP",
      "VARIANCE",
      "VAR_SAMP",
      "VAR_POP",
      "CORR",
      "COVAR_POP",
      "COVAR_SAMP",
      "LAG",
      "LEAD",
      "FIRST_VALUE",
      "LAST_VALUE",
      "NTH_VALUE",
      "PERCENTILE_CONT",
      "PERCENTILE_DISC",
  });
  std::ranges::sort(functions);
  return functions;
}();

std::vector<Implementation> Implementations(const std::vector<Rule>& rules,
                                            Implementation fallback) {
  std::set<Implementation> implementations;
  std::ranges::transform(
      rules, std::inserter(implementations, implementations.end()),
      [fallback](const Rule& rule) { return rule.implementation.value_or(fallback); });
  return {implementations.begin(), implementations.end()};
}

// Every function, collected from the rule tables. A function is in one table only.
const std::unordered_map<std::string_view, FunctionEntry>& Registry() {
  static const auto* const kRegistry = [] {
    auto* registry = new std::unordered_map<std::string_view, FunctionEntry>();
    const auto add = [registry](std::string_view name, FunctionEntry entry) {
      if (!registry->emplace(name, std::move(entry)).second) {
        std::cerr << "function " << name << " is registered twice\n";
        std::abort();
      }
    };
    for (const auto& [name, rules] : TemplateRules()) {
      add(name, {
                    .translation = Translation::kRules,
                    .implementations = Implementations(rules, Implementation::kSQL),
                    .rules = rules,
                });
    }
    for (const auto& [name, rules] : BackendRules()) {
      add(name, {
                    .translation = Translation::kBackend,
                    .implementations = Implementations(rules, Implementation::kGoogleSQL),
                    .rules = rules,
                });
    }
    for (const auto& [name, rules] : S2Rules()) {
      add(name, {
                    .translation = Translation::kS2,
                    .implementations = Implementations(rules, Implementation::kS2),
                    .rules = rules,
                });
    }
    for (const auto& [name, function] : kFunctionNames) {
      add(name, {
                    .translation = Translation::kRenamed,
                    .implementations = {function.implementation},
                    .target = function.target,
                });
    }
    for (const std::string_view name : kPlainFunctions) {
      add(name, {.translation = Translation::kSame, .implementations = {Implementation::kSame}});
    }
    for (const auto& [name, function] : kHandlers) {
      add(name, {
                    .translation = Translation::kHandler,
                    .implementations = {function.implementation},
                    .handler = function.handler,
                });
    }
    for (const auto& [name, target] : kSafeFunctions) {
      add(name, {
                    .translation = Translation::kSafe,
                    .implementations = {Implementation::kSafe},
                    .target = target,
                });
    }
    for (const auto& [name, rules] : Aggregates()) {
      add(name, {
                    .translation = Translation::kAggregate,
                    .implementations = {Implementation::kAggregate},
                    .aggregates = rules,
                });
    }
    for (const auto& [name, rules] : Analytics()) {
      add(name, {
                    .translation = Translation::kAnalytic,
                    .implementations = {Implementation::kAnalytic},
                    .aggregates = rules,
                });
    }
    return registry;
  }();
  return *kRegistry;
}

std::string_view Describe(Implementation implementation) {
  switch (implementation) {
    case Implementation::kSame:
      return "DuckDB function";
    case Implementation::kRenamed:
      return "DuckDB function, renamed";
    case Implementation::kSQL:
      return "DuckDB SQL";
    case Implementation::kGoogleSQL:
      return "GoogleSQL function";
    case Implementation::kS2:
      return "S2 function";
    case Implementation::kSQLInCode:
      return "DuckDB SQL, in code";
    case Implementation::kCustom:
      return "Emulator function";
    case Implementation::kSafe:
      return "SAFE. operator";
    case Implementation::kAggregate:
      return "DuckDB aggregate";
    case Implementation::kAnalytic:
      return "DuckDB window function";
  }
  return "";
}

}  // namespace

std::string Describe(const FunctionEntry& entry) {
  std::string description;
  for (const Implementation implementation : entry.implementations) {
    if (!description.empty()) {
      description += " / ";
    }
    description += Describe(implementation);
  }
  return description;
}

bool SupportsBigNumeric(std::string_view upper_name) {
  return std::ranges::binary_search(kBigNumericFunctions, upper_name);
}

const FunctionEntry* FindFunction(std::string_view upper_name) {
  const auto entry = Registry().find(upper_name);
  return entry == Registry().end() ? nullptr : &entry->second;
}

}  // namespace bigquery_emulator_duckdb::translator
