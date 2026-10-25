#pragma once

#include <optional>

#include "src/field_schema.h"

namespace googlesql {
class Value;
}

namespace bigquery_emulator_duckdb {

// Rounds a literal default with its column's current mode, including nested fields.
googlesql::Value RoundedColumnDefault(const FieldSchema& field, const googlesql::Value& value);

// Checks a literal column default against its REQUIRED mode and type parameters. A non-literal
// default has no `literal_value` and is rejected for parameterized fields.
void ValidateColumnDefault(const FieldSchema& field,
                           const std::optional<googlesql::Value>& literal_value);

}  // namespace bigquery_emulator_duckdb
