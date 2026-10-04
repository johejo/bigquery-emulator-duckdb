#pragma once

// DuckDB keeps a BIGNUMERIC as a BIGNUM, its integer number of units of 10^-38, which has the
// whole range and precision of BIGNUMERIC. Values cross between the two as the decimal text of
// that integer, which DuckDB casts to and from BIGNUM exactly.

#include <string>
#include <string_view>

#include "absl/status/statusor.h"
#include "googlesql/public/numeric_value.h"

namespace bigquery_emulator_duckdb {

// The decimal text of the units of `value`, such as "-150000000000000000000000000000000000000"
// for -1.5.
std::string BigNumericUnits(const googlesql::BigNumericValue& value);

// The BIGNUMERIC of `units`, the decimal text of an integer number of units. Fails for any other
// text, such as a DECIMAL with a decimal point, and for a value out of range.
absl::StatusOr<googlesql::BigNumericValue> BigNumericFromUnits(std::string_view units);

// The BIGNUMERIC of a BIGNUM as DuckDB stores it in a vector, which the C API has no accessor
// for. Fails for a value out of range.
absl::StatusOr<googlesql::BigNumericValue> BigNumericFromBignum(std::string_view stored);

// The DuckDB SQL of `value` as a BIGNUM.
std::string BigNumericSql(const googlesql::BigNumericValue& value);

}  // namespace bigquery_emulator_duckdb
