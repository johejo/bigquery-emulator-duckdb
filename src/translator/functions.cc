#include "src/translator/functions.h"

#include <cstdlib>
#include <iostream>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "src/translator/functions_aggregate.h"
#include "src/translator/functions_backend.h"
#include "src/translator/functions_template.h"
#include "src/translator/handlers.h"

namespace bigquery_emulator_duckdb::translator {
namespace {

// BigQuery functions that DuckDB has under another name, with the same arguments.
const std::unordered_map<std::string_view, std::string_view>& FunctionNames() {
  static const auto* const kNames = new std::unordered_map<std::string_view, std::string_view>{
      {"GENERATE_UUID", "uuid"},
      {"IS_INF", "isinf"},
      {"IS_NAN", "isnan"},
      {"RAND", "random"},
      {"SESSION_USER", "bq_session_user"},
      {"TIMESTAMP_SECONDS", "to_timestamp"},
      {"UNIX_MICROS", "epoch_us"},
  };
  return *kNames;
}

// BigQuery functions that DuckDB has under the same name, with the same arguments. Passing
// arbitrary builtin names through would accidentally accept internal functions and overloads
// with different semantics. Extend this list with execution coverage.
const std::unordered_set<std::string_view>& PlainFunctions() {
  static const auto* const kPlain = new std::unordered_set<std::string_view>{
      "IF",           "IFNULL", "NULLIF", "COALESCE", "CHAR_LENGTH", "CHARACTER_LENGTH",
      "ARRAY_LENGTH", "ATAN",   "ATAN2",  "TANH",     "ASINH",
  };
  return *kPlain;
}

// Functions that src/translator/handlers.cc translates in code.
const std::unordered_map<std::string_view, Handler>& Handlers() {
  static const auto* const kHandlers = new std::unordered_map<std::string_view, Handler>{
      {"$MAKE_ARRAY", MakeArray},
      {"$AND", Logical},
      {"$OR", Logical},
      {"$IN", InList},
      {"$CASE_NO_VALUE", Case},
      {"$CASE_WITH_VALUE", Case},
      {"DATE_BUCKET", Bucket},
      {"DATETIME_BUCKET", Bucket},
      {"TIMESTAMP_BUCKET", Bucket},
      {"TO_JSON", ToJson},
      {"TO_JSON_STRING", ToJson},
      {"JSON_ARRAY", JsonArray},
      {"JSON_REMOVE", JsonRemove},
      {"JSON_SET", JsonSet},
      {"JSON_ARRAY_APPEND", JsonArrayModify},
      {"JSON_ARRAY_INSERT", JsonArrayModify},
      {"JSON_OBJECT", JsonObject},
      {"FORMAT", Format},
      {"ARRAY_CONCAT", ArrayConcat},
      {"CONCAT", ConcatStrings},
      {"GREATEST", Extremum},
      {"LEAST", Extremum},
  };
  return *kHandlers;
}

// SAFE_ADD and its siblings are the arithmetic operators with the SAFE. prefix.
const std::unordered_map<std::string_view, std::string_view>& SafeFunctions() {
  static const auto* const kSafe = new std::unordered_map<std::string_view, std::string_view>{
      {"SAFE_ADD", "$ADD"},
      {"SAFE_SUBTRACT", "$SUBTRACT"},
      {"SAFE_MULTIPLY", "$MULTIPLY"},
      {"SAFE_NEGATE", "$UNARY_MINUS"},
  };
  return *kSafe;
}

// The functions that handle BIGNUMERIC, by carrying it as it is, by comparing BIGNUM, which
// DuckDB does exactly, or by rules of their own.
const std::unordered_set<std::string_view>& BigNumericFunctions() {
  static const auto* const kFunctions = new std::unordered_set<std::string_view>{
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
      "APPROX_QUANTILES",
      "APPROX_TOP_COUNT",
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
  };
  return *kFunctions;
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
      add(name, {.implementation = Implementation::kRules, .rules = rules});
    }
    for (const auto& [name, rules] : BackendRules()) {
      add(name, {.implementation = Implementation::kBackend, .rules = rules});
    }
    for (const auto& [name, target] : FunctionNames()) {
      add(name, {.implementation = Implementation::kRenamed, .target = target});
    }
    for (const std::string_view name : PlainFunctions()) {
      add(name, {.implementation = Implementation::kSame});
    }
    for (const auto& [name, handler] : Handlers()) {
      add(name, {.implementation = Implementation::kHandler, .handler = handler});
    }
    for (const auto& [name, target] : SafeFunctions()) {
      add(name, {.implementation = Implementation::kSafe, .target = target});
    }
    for (const auto& [name, rules] : Aggregates()) {
      add(name, {.implementation = Implementation::kAggregate, .aggregates = rules});
    }
    for (const auto& [name, rules] : Analytics()) {
      add(name, {.implementation = Implementation::kAnalytic, .aggregates = rules});
    }
    return registry;
  }();
  return *kRegistry;
}

}  // namespace

std::string_view Describe(Implementation implementation) {
  switch (implementation) {
    case Implementation::kSame:
      return "DuckDB function";
    case Implementation::kRenamed:
      return "DuckDB function, renamed";
    case Implementation::kRules:
      return "DuckDB SQL";
    case Implementation::kBackend:
      return "GoogleSQL function";
    case Implementation::kHandler:
      return "DuckDB SQL, in code";
    case Implementation::kSafe:
      return "SAFE. operator";
    case Implementation::kAggregate:
      return "DuckDB aggregate";
    case Implementation::kAnalytic:
      return "DuckDB window function";
  }
  return "";
}

bool SupportsBigNumeric(std::string_view upper_name) {
  return BigNumericFunctions().contains(upper_name);
}

const FunctionEntry* FindFunction(std::string_view upper_name) {
  const auto entry = Registry().find(upper_name);
  return entry == Registry().end() ? nullptr : &entry->second;
}

}  // namespace bigquery_emulator_duckdb::translator
