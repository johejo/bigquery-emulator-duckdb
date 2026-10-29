#pragma once

#include <optional>
#include <string>

#include "src/timestamp_precision.h"

namespace googlesql {
class Value;
}  // namespace googlesql

namespace bigquery_emulator_duckdb::translator {

// Literals, in literal.cc.

std::optional<std::string> Literal(
    const googlesql::Value& value,
    TimestampPrecision precision = TimestampPrecision::kMicroseconds);

}  // namespace bigquery_emulator_duckdb::translator
