#include <algorithm>
#include <span>
#include <string_view>

#include "src/translator/functions.h"

namespace bigquery_emulator_duckdb::translator {
namespace {

constexpr std::string_view kCarries[] = {
    "$IS_NULL",
    "$CASE_NO_VALUE",
    "IF",
    "IFNULL",
    "COALESCE",
    "$MAKE_ARRAY",
    "$ARRAY_AT_OFFSET",
    "$ARRAY_AT_ORDINAL",
    "$SAFE_ARRAY_AT_OFFSET",
    "$SAFE_ARRAY_AT_ORDINAL",
    "ARRAY_LENGTH",
    "ARRAY_REVERSE",
    "ARRAY_CONCAT",
};

constexpr std::string_view kCompares[] = {
    "$EQUAL",
    "$NOT_EQUAL",
    "$LESS",
    "$LESS_OR_EQUAL",
    "$GREATER",
    "$GREATER_OR_EQUAL",
    "$BETWEEN",
    "$IS_DISTINCT_FROM",
    "$IS_NOT_DISTINCT_FROM",
    "$IN",
    "$IN_ARRAY",
    "$CASE_WITH_VALUE",
    "NULLIF",
    "GREATEST",
    "LEAST",
};

constexpr std::string_view kBuildsOrReadsInterval[] = {
    "$ADD",           "$SUBTRACT",         "$UNARY_MINUS",      "$MULTIPLY",
    "$DIVIDE",        "$INTERVAL",         "$EXTRACT",          "MAKE_INTERVAL",
    "JUSTIFY_HOURS",  "JUSTIFY_DAYS",      "JUSTIFY_INTERVAL",  "ERROR",
    "FORMAT",         "JSON_ARRAY_APPEND", "JSON_ARRAY_INSERT", "TO_JSON",
    "TO_JSON_STRING", "JSON_ARRAY",        "JSON_OBJECT",       "GENERATE_RANGE_ARRAY",
};

constexpr std::string_view kComparesIntervalKeys[] = {
    "$EQUAL",
    "$NOT_EQUAL",
    "$LESS",
    "$LESS_OR_EQUAL",
    "$GREATER",
    "$GREATER_OR_EQUAL",
    "$BETWEEN",
    "$IS_DISTINCT_FROM",
    "$IS_NOT_DISTINCT_FROM",
    "$IN",
    "$IN_ARRAY",
    "RANGE_BUCKET",
};

constexpr std::string_view kComparesStructs[] = {
    "$EQUAL", "$NOT_EQUAL", "$IN", "$IN_ARRAY", "$CASE_WITH_VALUE", "NULLIF",
};

constexpr std::string_view kComparesStructsWithInterval[] = {
    "$EQUAL",
    "$NOT_EQUAL",
    "$IN",
    "$IN_ARRAY",
};

constexpr std::string_view kBuildsOrReadsRange[] = {
    "FORMAT",
    "TO_JSON",
    "TO_JSON_STRING",
    "JSON_ARRAY",
    "JSON_OBJECT",
    "JSON_SET",
    "JSON_ARRAY_APPEND",
    "JSON_ARRAY_INSERT",
    "RANGE",
    "RANGE_START",
    "RANGE_END",
    "RANGE_CONTAINS",
    "RANGE_OVERLAPS",
    "RANGE_INTERSECT",
    "GENERATE_RANGE_ARRAY",
    "ERROR",
};

constexpr std::string_view kAggregatesRange[] = {
    "COUNT",      "MIN",       "MAX", "ANY_VALUE", "ARRAY_AGG", "FIRST_VALUE",
    "LAST_VALUE", "NTH_VALUE", "LAG", "LEAD",      "MIN_BY",    "MAX_BY",
};

constexpr std::string_view kReadsEmptyStruct[] = {
    "TO_JSON", "TO_JSON_STRING",    "JSON_ARRAY",        "JSON_OBJECT", "JSON_SET",
    "FORMAT",  "JSON_ARRAY_APPEND", "JSON_ARRAY_INSERT", "$MAKE_ARRAY",
};

constexpr std::string_view kRejectsSafe[] = {
    "SESSION_USER",
    "ERROR",
    "TEXT_ANALYZE",
    "ST_GEOHASH",
};

std::span<const std::string_view> Functions(TypeUse use) {
  switch (use) {
    case TypeUse::kCarries:
      return kCarries;
    case TypeUse::kCompares:
      return kCompares;
    case TypeUse::kBuildsOrReadsInterval:
      return kBuildsOrReadsInterval;
    case TypeUse::kComparesIntervalKeys:
      return kComparesIntervalKeys;
    case TypeUse::kComparesStructs:
      return kComparesStructs;
    case TypeUse::kComparesStructsWithInterval:
      return kComparesStructsWithInterval;
    case TypeUse::kBuildsOrReadsRange:
      return kBuildsOrReadsRange;
    case TypeUse::kAggregatesRange:
      return kAggregatesRange;
    case TypeUse::kReadsEmptyStruct:
      return kReadsEmptyStruct;
    case TypeUse::kRejectsSafe:
      return kRejectsSafe;
  }
  return {};
}

}  // namespace

bool HasTypeUse(std::string_view upper_name, TypeUse use) {
  return std::ranges::find(Functions(use), upper_name) != Functions(use).end();
}

}  // namespace bigquery_emulator_duckdb::translator
