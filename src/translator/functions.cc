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
  TypeUses uses = {};
};

// Functions called by another name, with the same arguments.
constexpr auto kFunctionNames = std::to_array<std::pair<std::string_view, NamedFunction>>({
    {"GENERATE_UUID", {.target = "uuid"}},
    {"IS_INF", {.target = "isinf"}},
    {"IS_NAN", {.target = "isnan"}},
    {"RAND", {.target = "random"}},
    {
        "SESSION_USER",
        {
            .target = "bq_session_user",
            .implementation = Implementation::kCustom,
            .uses = {TypeUse::kRejectsSafe},
        },
    },
    {"UNIX_MICROS", {.target = "epoch_us"}},
});

// BigQuery functions that DuckDB has under the same name, with the same arguments. Passing
// arbitrary builtin names through would accidentally accept internal functions and overloads
// with different semantics. Extend this list with execution coverage.
struct PlainFunction {
  std::string_view name;
  TypeUses uses = {};
};

constexpr auto kPlainFunctions = std::to_array<PlainFunction>({
    {.name = "IFNULL", .uses = {TypeUse::kCarries, TypeUse::kGraphValues, TypeUse::kBigNumeric}},
    {.name = "COALESCE", .uses = {TypeUse::kCarries, TypeUse::kGraphValues, TypeUse::kBigNumeric}},
    {.name = "CHAR_LENGTH"},
    {.name = "CHARACTER_LENGTH"},
    {
        .name = "ARRAY_LENGTH",
        .uses = {TypeUse::kCarries, TypeUse::kGraphValues, TypeUse::kBigNumeric},
    },
});

struct HandledFunction {
  Handler handler;
  Implementation implementation = Implementation::kSQLInCode;
  TypeUses uses = {};
};

// Functions that src/translator/handlers.cc translates in code.
constexpr auto kHandlers =
    std::to_array<std::pair<std::string_view, HandledFunction>>(
        {
            {
                "$MAKE_ARRAY",
                {
                    .handler = MakeArray,
                    .uses = {TypeUse::kCarries, TypeUse::kReadsEmptyStruct, TypeUse::kBigNumeric},
                },
            },
            {"$AND", {.handler = Logical}},
            {"$OR", {.handler = Logical}},
            {
                "$IN",
                {
                    .handler = InList,
                    .uses =
                        {
                            TypeUse::kCompares,
                            TypeUse::kComparesIntervalKeys,
                            TypeUse::kComparesStructs,
                            TypeUse::kComparesStructsWithInterval,
                            TypeUse::kBigNumeric,
                        },
                },
            },
            {
                "$CASE_NO_VALUE",
                {
                    .handler = Case,
                    .uses = {TypeUse::kCarries, TypeUse::kGraphValues, TypeUse::kBigNumeric},
                },
            },
            {
                "$CASE_WITH_VALUE",
                {
                    .handler = Case,
                    .uses =
                        {
                            TypeUse::kCompares,
                            TypeUse::kComparesStructs,
                            TypeUse::kComparesStructsWithInterval,
                            TypeUse::kBigNumeric,
                        },
                },
            },
            {"SAME", {.handler = GraphIdentityPredicate, .uses = {TypeUse::kGraphValues}}},
            {"ALL_DIFFERENT", {.handler = GraphIdentityPredicate, .uses = {TypeUse::kGraphValues}}},
            {"DATE_BUCKET", {.handler = Bucket}},
            {"DATETIME_BUCKET", {.handler = Bucket}},
            {"TIMESTAMP_BUCKET", {.handler = Bucket}},
            {
                "TO_JSON",
                {
                    .handler = ToJson,
                    .implementation = Implementation::kGoogleSQL,
                    .uses =
                        {
                            TypeUse::kBuildsOrReadsInterval,
                            TypeUse::kBuildsOrReadsRange,
                            TypeUse::kReadsEmptyStruct,
                            TypeUse::kBigNumeric,
                        },
                },
            },
            {
                "TO_JSON_STRING",
                {
                    .handler = ToJson,
                    .implementation = Implementation::kGoogleSQL,
                    .uses =
                        {
                            TypeUse::kBuildsOrReadsInterval,
                            TypeUse::kBuildsOrReadsRange,
                            TypeUse::kReadsEmptyStruct,
                            TypeUse::kBigNumeric,
                        },
                },
            },
            {
                "JSON_ARRAY",
                {
                    .handler = JsonArray,
                    .implementation = Implementation::kGoogleSQL,
                    .uses =
                        {
                            TypeUse::kBuildsOrReadsInterval,
                            TypeUse::kBuildsOrReadsRange,
                            TypeUse::kReadsEmptyStruct,
                            TypeUse::kBigNumeric,
                        },
                },
            },
            {"JSON_REMOVE", {.handler = JsonRemove, .implementation = Implementation::kGoogleSQL}},
            {
                "JSON_SET",
                {
                    .handler = JsonSet,
                    .implementation = Implementation::kGoogleSQL,
                    .uses =
                        {
                            TypeUse::kBuildsOrReadsInterval,
                            TypeUse::kBuildsOrReadsRange,
                            TypeUse::kReadsEmptyStruct,
                            TypeUse::kBigNumeric,
                        },
                },
            },
            {
                "JSON_ARRAY_APPEND",
                {
                    .handler = JsonArrayModify,
                    .implementation = Implementation::kGoogleSQL,
                    .uses =
                        {
                            TypeUse::kBuildsOrReadsInterval,
                            TypeUse::kBuildsOrReadsRange,
                            TypeUse::kReadsEmptyStruct,
                            TypeUse::kBigNumeric,
                        },
                },
            },
            {
                "JSON_ARRAY_INSERT",
                {
                    .handler = JsonArrayModify,
                    .implementation = Implementation::kGoogleSQL,
                    .uses =
                        {
                            TypeUse::kBuildsOrReadsInterval,
                            TypeUse::kBuildsOrReadsRange,
                            TypeUse::kReadsEmptyStruct,
                            TypeUse::kBigNumeric,
                        },
                },
            },
            {
                "JSON_OBJECT",
                {
                    .handler = JsonObject,
                    .implementation = Implementation::kGoogleSQL,
                    .uses =
                        {
                            TypeUse::kBuildsOrReadsInterval,
                            TypeUse::kBuildsOrReadsRange,
                            TypeUse::kReadsEmptyStruct,
                            TypeUse::kBigNumeric,
                        },
                },
            },
            {
                "FORMAT",
                {
                    .handler = Format,
                    .implementation = Implementation::kGoogleSQL,
                    .uses =
                        {
                            TypeUse::kBuildsOrReadsInterval,
                            TypeUse::kBuildsOrReadsRange,
                            TypeUse::kReadsEmptyStruct,
                            TypeUse::kBigNumeric,
                        },
                },
            },
            {
                "ARRAY_CONCAT",
                {.handler = ArrayConcat, .uses = {TypeUse::kCarries, TypeUse::kBigNumeric}},
            },
            {"CONCAT", {.handler = ConcatStrings}},
            {
                "GREATEST",
                {
                    .handler = Extremum,
                    .uses =
                        {
                            TypeUse::kCompares,
                            TypeUse::kRejectsIntervalComparison,
                            TypeUse::kBigNumeric,
                        },
                },
            },
            {
                "LEAST",
                {
                    .handler = Extremum,
                    .uses =
                        {
                            TypeUse::kCompares,
                            TypeUse::kRejectsIntervalComparison,
                            TypeUse::kBigNumeric,
                        },
                },
            },
            {
                "KEYS.ADD_KEY_FROM_RAW_BYTES",
                {.handler = AddKeyFromRawBytes, .implementation = Implementation::kGoogleSQL},
            },
            {
                "TEXT_ANALYZE",
                {
                    .handler = TextAnalyze,
                    .implementation = Implementation::kGoogleSQL,
                    .uses = {TypeUse::kRejectsSafe},
                },
            },
            {"OBJ.MAKE_REF", {.handler = MakeRef, .implementation = Implementation::kCustom}},
            {
                "OBJ.FETCH_METADATA",
                {.handler = FetchMetadata, .implementation = Implementation::kCustom},
            },
        });

// SAFE_ADD and its siblings are the arithmetic operators with the SAFE. prefix.
constexpr auto kSafeFunctions = std::to_array<std::pair<std::string_view, std::string_view>>({
    {"SAFE_ADD", "$ADD"},
    {"SAFE_SUBTRACT", "$SUBTRACT"},
    {"SAFE_MULTIPLY", "$MULTIPLY"},
    {"SAFE_NEGATE", "$UNARY_MINUS"},
});

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
    for (const auto& [name, definition] : TemplateRules()) {
      add(name, {
                    .translation = Translation::kRules,
                    .uses = definition.uses,
                    .implementations = Implementations(definition.rules, Implementation::kSQL),
                    .rules = definition.rules,
                });
    }
    for (const auto& [name, definition] : BackendRules()) {
      add(name,
          {
              .translation = Translation::kBackend,
              .uses = definition.uses,
              .implementations = Implementations(definition.rules, Implementation::kGoogleSQL),
              .rules = definition.rules,
          });
    }
    for (const auto& [name, definition] : S2Rules()) {
      add(name, {
                    .translation = Translation::kS2,
                    .uses = definition.uses,
                    .implementations = Implementations(definition.rules, Implementation::kS2),
                    .rules = definition.rules,
                });
    }
    for (const auto& [name, function] : kFunctionNames) {
      add(name, {
                    .translation = Translation::kRenamed,
                    .uses = function.uses,
                    .implementations = {function.implementation},
                    .target = function.target,
                });
    }
    for (const auto& function : kPlainFunctions) {
      add(function.name, {
                             .translation = Translation::kSame,
                             .uses = function.uses,
                             .implementations = {Implementation::kSame},
                         });
    }
    for (const auto& [name, function] : kHandlers) {
      add(name, {
                    .translation = Translation::kHandler,
                    .uses = function.uses,
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
    for (const auto& [name, definition] : Aggregates()) {
      add(name, {
                    .translation = Translation::kAggregate,
                    .uses = definition.uses,
                    .implementations = {Implementation::kAggregate},
                    .aggregates = definition.rules,
                });
    }
    for (const auto& [name, definition] : Analytics()) {
      add(name, {
                    .translation = Translation::kAnalytic,
                    .uses = definition.uses,
                    .implementations = {Implementation::kAnalytic},
                    .aggregates = definition.rules,
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

bool HasTypeUse(std::string_view upper_name, TypeUse use) {
  const FunctionEntry* entry = FindFunction(upper_name);
  return entry != nullptr && entry->uses.Contains(use);
}

const FunctionEntry* FindFunction(std::string_view upper_name) {
  const auto entry = Registry().find(upper_name);
  return entry == Registry().end() ? nullptr : &entry->second;
}

}  // namespace bigquery_emulator_duckdb::translator
