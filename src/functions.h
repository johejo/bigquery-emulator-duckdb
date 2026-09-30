#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bigquery_emulator_duckdb {

// The argument types the translation rules tell apart. Everything else is kOther.
enum class ArgumentType : std::uint8_t {
  kOther,
  kString,
  kBytes,
  kInt64,
  kDate,
  kDatetime,
  kTime,
  kTimestamp,
  kJson,
};

// An argument of a BigQuery function call, as far as the translation rules look at it.
struct FunctionArgument {
  // The argument translated to DuckDB SQL.
  std::string sql;
  ArgumentType type = ArgumentType::kOther;
  // The date part, such as "day", when the argument is one.
  std::optional<std::string> date_part;
  // The rounding mode, such as "ROUND_HALF_EVEN", when the argument is one.
  std::optional<std::string> rounding_mode;
  // The value of a STRING literal argument.
  std::optional<std::string> string_literal;
};

// The DuckDB spelling of a call to the BigQuery scalar function `upper_name`, or nullopt when
// the declarative rules do not translate it. `upper_name` is the function name in upper case,
// and the operators have their internal names such as $EXTRACT_DATE.
std::optional<std::string> TranslateFunction(std::string_view upper_name,
                                             const std::vector<FunctionArgument>& arguments);

}  // namespace bigquery_emulator_duckdb
