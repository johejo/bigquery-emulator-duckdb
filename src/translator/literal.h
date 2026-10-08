#pragma once

#include <optional>
#include <string>

namespace googlesql {
class Value;
}  // namespace googlesql

namespace bigquery_emulator_duckdb::translator {

// Literals, in literal.cc.

std::optional<std::string> Literal(const googlesql::Value& value);

}  // namespace bigquery_emulator_duckdb::translator
