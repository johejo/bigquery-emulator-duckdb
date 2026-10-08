#pragma once

#include <optional>
#include <string>

#include "src/translator/functions.h"

namespace bigquery_emulator_duckdb::translator {

// The handlers that the registry in functions.cc names, in function.cc.
std::optional<std::string> MakeArray(const ScalarCall& call);
std::optional<std::string> Logical(const ScalarCall& call);
std::optional<std::string> InList(const ScalarCall& call);
std::optional<std::string> Case(const ScalarCall& call);
std::optional<std::string> Bucket(const ScalarCall& call);
std::optional<std::string> ToJson(const ScalarCall& call);
std::optional<std::string> JsonRemove(const ScalarCall& call);
std::optional<std::string> JsonSet(const ScalarCall& call);
std::optional<std::string> JsonArray(const ScalarCall& call);
std::optional<std::string> JsonObject(const ScalarCall& call);
std::optional<std::string> ArrayConcat(const ScalarCall& call);
std::optional<std::string> ConcatStrings(const ScalarCall& call);
std::optional<std::string> Format(const ScalarCall& call);
std::optional<std::string> Extremum(const ScalarCall& call);

}  // namespace bigquery_emulator_duckdb::translator
