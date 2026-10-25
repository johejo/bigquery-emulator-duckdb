#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace googlesql {
class Type;
}  // namespace googlesql

namespace bigquery_emulator_duckdb::function_catalog {

// SQL comparison helpers shared by expression translation and function handlers.

// Field-by-field equality, preserving BigQuery's NULL, NaN and INTERVAL comparisons.
std::string Equal(const googlesql::Type* type, const std::string& left, const std::string& right);

// `body` over the arguments `args` that `indexes` name, each bound once as _sc.aN.
std::string BindComparison(const std::vector<std::string>& args,
                           const std::vector<std::size_t>& indexes, const std::string& body);
std::string BoundComparison(std::size_t i);

// `value` IN UNNEST(`array`) for a STRUCT `type`.
std::string StructInArray(const googlesql::Type* type, const std::string& value,
                          const std::string& array);

}  // namespace bigquery_emulator_duckdb::function_catalog
