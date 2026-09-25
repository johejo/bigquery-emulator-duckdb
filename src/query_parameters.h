#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "nlohmann/json_fwd.hpp"
#include "src/field_schema.h"

namespace bigquery_emulator_duckdb {

// The query parameters of a request, with every value already converted to the DuckDB literal
// that replaces the parameter in the translated query.
//
// A query names its parameters either by name (`@name`) or by position (`?`); BigQuery calls
// that the parameter mode and a request uses one or the other.
class QueryParameters {
 public:
  QueryParameters() = default;

  // Parses BigQuery's QueryParameter list. Throws ApiError when a parameter is malformed or
  // has a type the emulator does not support.
  static QueryParameters Parse(const nlohmann::json& parameters);

  bool empty() const { return by_name_.empty() && by_position_.empty(); }

  // The literal for `@name` and for the `position`-th `?` (1-based). Both throw ApiError when
  // the query uses a parameter the request did not declare.
  const std::string& ByName(const std::string& name) const;
  const std::string& ByPosition(int position) const;

  // The declared types, in TableFieldSchema form: the named parameters with their names, in
  // request order, and the positional ones in position order. An ARRAY parameter is its
  // element type with mode REPEATED.
  const std::vector<FieldSchema>& named_types() const { return named_types_; }
  const std::vector<FieldSchema>& positional_types() const { return positional_types_; }

 private:
  std::unordered_map<std::string, std::string> by_name_;  // Keyed by the lower-cased name.
  std::vector<std::string> by_position_;
  std::vector<FieldSchema> named_types_;
  std::vector<FieldSchema> positional_types_;
};

}  // namespace bigquery_emulator_duckdb
