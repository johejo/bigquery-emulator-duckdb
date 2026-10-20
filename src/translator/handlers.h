#pragma once

#include <optional>
#include <string>

#include "src/translator/functions.h"

namespace googlesql {
class Type;
}  // namespace googlesql

namespace bigquery_emulator_duckdb::translator {

// The handlers that the registry in functions.cc names.
std::optional<std::string> MakeArray(const ScalarCall& call);
std::optional<std::string> Logical(const ScalarCall& call);
std::optional<std::string> InList(const ScalarCall& call);
std::optional<std::string> Case(const ScalarCall& call);
std::optional<std::string> Bucket(const ScalarCall& call);
std::optional<std::string> ToJson(const ScalarCall& call);
std::optional<std::string> JsonRemove(const ScalarCall& call);
std::optional<std::string> JsonSet(const ScalarCall& call);
std::optional<std::string> JsonArrayModify(const ScalarCall& call);
std::optional<std::string> JsonArray(const ScalarCall& call);
std::optional<std::string> JsonObject(const ScalarCall& call);
std::optional<std::string> ArrayConcat(const ScalarCall& call);
std::optional<std::string> ConcatStrings(const ScalarCall& call);
std::optional<std::string> Format(const ScalarCall& call);
std::optional<std::string> Extremum(const ScalarCall& call);
std::optional<std::string> AddKeyFromRawBytes(const ScalarCall& call);
std::optional<std::string> TextAnalyze(const ScalarCall& call);
std::optional<std::string> MakeRef(const ScalarCall& call);
std::optional<std::string> FetchMetadata(const ScalarCall& call);

// Equality, IN, CASE with a value and NULLIF over STRUCTs, which BigQuery compares field by field
// with NULL fields propagating NULL; Call in function.cc takes these calls from their rules.
std::optional<std::string> StructComparison(const ScalarCall& call);
// `value` IN UNNEST(`array`) for a STRUCT `type`.
std::string StructInArray(const googlesql::Type* type, const std::string& value,
                          const std::string& array);

}  // namespace bigquery_emulator_duckdb::translator
