#include "src/translator/functions.h"

#include <array>
#include <span>
#include <string_view>
#include <utility>

#include "src/function_catalog/catalog.h"
#include "src/function_catalog/rules.h"
#include "src/translator/functions_aggregate.h"
#include "src/translator/functions_backend.h"
#include "src/translator/functions_template.h"
#include "src/translator/handlers.h"

namespace bigquery_emulator_duckdb::translator {

using function_catalog::ArgumentMode;
using function_catalog::Handler;
using function_catalog::Implementation;
using function_catalog::Translation;
using function_catalog::TypeRejection;
using function_catalog::TypeUse;
using function_catalog::TypeUses;

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
  std::span<const ArgumentMode> argument_modes = {};
  Handler struct_handler = nullptr;
  std::span<const TypeRejection> type_rejections = {};
};

constexpr auto kBucketArgumentModes =
    std::to_array<ArgumentMode>({ArgumentMode::kExpression, ArgumentMode::kBucketWidth});

constexpr std::array kIntervalTypes = {googlesql::TYPE_INTERVAL};

constexpr auto kCaseTypeRejections = std::to_array<TypeRejection>({
    {
        .types = kIntervalTypes,
        .message = "Unsupported type in function CASE_WITH_VALUE",
    },
});

constexpr auto kGreatestTypeRejections = std::to_array<TypeRejection>({
    {
        .types = kIntervalTypes,
        .message = "Unsupported type in function GREATEST",
    },
});

constexpr auto kLeastTypeRejections = std::to_array<TypeRejection>({
    {
        .types = kIntervalTypes,
        .message = "Unsupported type in function LEAST",
    },
});

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
                            TypeUse::kComparesStructsWithInterval,
                            TypeUse::kBigNumeric,
                        },
                    .struct_handler = StructComparison,
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
                            TypeUse::kCarriesResults,
                            TypeUse::kComparesStructsWithInterval,
                            TypeUse::kBigNumeric,
                        },
                    .struct_handler = StructComparison,
                    .type_rejections = kCaseTypeRejections,
                },
            },
            {"SAME", {.handler = GraphIdentityPredicate, .uses = {TypeUse::kGraphValues}}},
            {"ALL_DIFFERENT", {.handler = GraphIdentityPredicate, .uses = {TypeUse::kGraphValues}}},
            {"DATE_BUCKET", {.handler = Bucket, .argument_modes = kBucketArgumentModes}},
            {"DATETIME_BUCKET", {.handler = Bucket, .argument_modes = kBucketArgumentModes}},
            {"TIMESTAMP_BUCKET", {.handler = Bucket, .argument_modes = kBucketArgumentModes}},
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
                            TypeUse::kBigNumeric,
                        },
                    .type_rejections = kGreatestTypeRejections,
                },
            },
            {
                "LEAST",
                {
                    .handler = Extremum,
                    .uses =
                        {
                            TypeUse::kCompares,
                            TypeUse::kBigNumeric,
                        },
                    .type_rejections = kLeastTypeRejections,
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

}  // namespace

void AddBuiltinFunctions(function_catalog::Catalog& functions) {
  for (const auto& [name, definition] : TemplateRules()) {
    functions.AddRules(name, definition, Implementation::kSQL);
  }
  for (const auto& [name, definition] : BackendRules()) {
    functions.AddRules(name, definition, Implementation::kGoogleSQL);
  }
  for (const auto& [name, function] : kFunctionNames) {
    functions.Add(name, {
                            .translation = Translation::kRenamed,
                            .uses = function.uses,
                            .implementations = {function.implementation},
                            .target = function.target,
                        });
  }
  for (const auto& function : kPlainFunctions) {
    functions.Add(function.name, {
                                     .translation = Translation::kSame,
                                     .uses = function.uses,
                                     .implementations = {Implementation::kSame},
                                 });
  }
  for (const auto& [name, function] : kHandlers) {
    functions.Add(name, {
                            .translation = Translation::kHandler,
                            .uses = function.uses,
                            .implementations = {function.implementation},
                            .handler = function.handler,
                            .argument_modes =
                                {
                                    function.argument_modes.begin(),
                                    function.argument_modes.end(),
                                },
                            .struct_handler = function.struct_handler,
                            .type_rejections =
                                {
                                    function.type_rejections.begin(),
                                    function.type_rejections.end(),
                                },
                        });
  }
  for (const auto& [name, target] : kSafeFunctions) {
    functions.Add(name, {
                            .translation = Translation::kSafe,
                            .implementations = {Implementation::kSafe},
                            .target = target,
                        });
  }
  for (const auto& [name, definition] : Aggregates()) {
    functions.Add(name, {
                            .translation = Translation::kAggregate,
                            .uses = definition.uses,
                            .implementations = {Implementation::kAggregate},
                            .type_rejections = definition.type_rejections,
                            .aggregates = definition.rules,
                        });
  }
  for (const auto& [name, definition] : Analytics()) {
    functions.Add(name, {
                            .translation = Translation::kAnalytic,
                            .uses = definition.uses,
                            .implementations = {Implementation::kAnalytic},
                            .type_rejections = definition.type_rejections,
                            .aggregates = definition.rules,
                        });
  }
}

}  // namespace bigquery_emulator_duckdb::translator
