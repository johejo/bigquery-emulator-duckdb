#pragma once

// DuckDB keeps a BIGNUMERIC as a BIGNUM, its integer number of units of 10^-38, which has the
// whole range and precision of BIGNUMERIC. Values cross between the two as the decimal text of
// that integer, which DuckDB casts to and from BIGNUM exactly.

#include <cstdint>
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

// The BIGNUMERIC of a Parquet DECIMAL of `scale`, the big-endian two's complement of its
// integer number of units of 10^-scale. Fails for a scale greater than BIGNUMERIC's and for a
// value out of range.
absl::StatusOr<googlesql::BigNumericValue> BigNumericFromDecimalBytes(std::string_view bytes,
                                                                      int64_t scale);

// The 32 bytes of `value` as a Parquet DECIMAL(76, 38), the big-endian two's complement of its
// units.
std::string BigNumericDecimalBytes(const googlesql::BigNumericValue& value);

// The DuckDB SQL of `value` as a BIGNUM.
std::string BigNumericSql(const googlesql::BigNumericValue& value);

}  // namespace bigquery_emulator_duckdb
