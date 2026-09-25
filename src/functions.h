#pragma once

#include <cstddef>
#include <optional>
#include <string_view>

namespace bigquery_emulator_duckdb {

// The DuckDB name of a BigQuery function that differs from it only in name, or nullopt when the
// name needs no change. `upper_name` is the BigQuery function name in upper case. Only scalar
// functions are renamed here; aggregate and analytic functions have their own mapping.
std::optional<std::string_view> DuckDbFunctionName(std::string_view upper_name);

// The DuckDB spelling of a BigQuery function whose call has to be restructured, as a template
// over the arguments, or nullopt when no rule applies to this name and argument count:
//
//   $n  the n-th argument, translated to DuckDB SQL,
//   #n  the n-th argument, a BigQuery date part such as DAY, as the lower case string literal
//       that DuckDB expects ('day').
std::optional<std::string_view> DuckDbFunctionTemplate(std::string_view upper_name,
                                                       std::size_t argument_count);

}  // namespace bigquery_emulator_duckdb
