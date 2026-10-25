#pragma once

#include <optional>
#include <string>

#include "src/function_catalog/catalog.h"

namespace bigquery_emulator_duckdb::translator {

// The handlers that the registry in functions.cc names.
std::optional<std::string> MakeArray(const function_catalog::ScalarCall& call);
std::optional<std::string> Logical(const function_catalog::ScalarCall& call);
std::optional<std::string> InList(const function_catalog::ScalarCall& call);
std::optional<std::string> Case(const function_catalog::ScalarCall& call);
std::optional<std::string> GraphIdentityPredicate(const function_catalog::ScalarCall& call);
std::optional<std::string> Bucket(const function_catalog::ScalarCall& call);
std::optional<std::string> ToJson(const function_catalog::ScalarCall& call);
std::optional<std::string> JsonRemove(const function_catalog::ScalarCall& call);
std::optional<std::string> JsonSet(const function_catalog::ScalarCall& call);
std::optional<std::string> JsonArrayModify(const function_catalog::ScalarCall& call);
std::optional<std::string> JsonArray(const function_catalog::ScalarCall& call);
std::optional<std::string> JsonObject(const function_catalog::ScalarCall& call);
std::optional<std::string> ArrayConcat(const function_catalog::ScalarCall& call);
std::optional<std::string> ConcatStrings(const function_catalog::ScalarCall& call);
std::optional<std::string> Format(const function_catalog::ScalarCall& call);
std::optional<std::string> Extremum(const function_catalog::ScalarCall& call);
std::optional<std::string> AddKeyFromRawBytes(const function_catalog::ScalarCall& call);
std::optional<std::string> TextAnalyze(const function_catalog::ScalarCall& call);
std::optional<std::string> MakeRef(const function_catalog::ScalarCall& call);
std::optional<std::string> FetchMetadata(const function_catalog::ScalarCall& call);

// Equality, IN, CASE with a value and NULLIF over STRUCTs, which BigQuery compares field by field
// with NULL fields propagating NULL; registrations select this for a STRUCT first argument.
std::optional<std::string> StructComparison(const function_catalog::ScalarCall& call);

}  // namespace bigquery_emulator_duckdb::translator
