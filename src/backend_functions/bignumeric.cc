#include "src/bignumeric.h"

#include <cstdint>
#include <optional>
#include <string>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "duckdb.h"
#include "googlesql/public/functions/arithmetics.h"
#include "googlesql/public/functions/convert.h"
#include "googlesql/public/functions/convert_string.h"
#include "googlesql/public/numeric_value.h"
#include "src/backend_functions/internal.h"

// BIGNUMERIC arithmetic and conversions. The translator passes a BIGNUM to these functions, and
// takes one back, as the VARCHAR of its units; see src/bignumeric.h. A conversion's last argument
// is true under SAFE_CAST, which makes its errors NULL.

namespace bigquery_emulator_duckdb::backend_functions {
namespace {

namespace fn = googlesql::functions;
using googlesql::BigNumericValue;

// Turns a GoogleSQL function's out parameter and error into a StatusOr, with an error as NULL
// when `safe` is true.
template <typename T>
absl::StatusOr<std::optional<T>> OrNull(bool ok, T value, const absl::Status& error, bool safe) {
  if (ok) {
    return value;
  }
  if (safe) {
    return std::nullopt;
  }
  return error;
}

// The BIGNUMERIC of argument `column`, which holds the units of a BIGNUM.
absl::StatusOr<BigNumericValue> Units(const Arguments& arguments, idx_t column) {
  return BigNumericFromUnits(arguments.String(column));
}

// A BIGNUMERIC operator of two arguments, such as +.
template <bool (*kFunction)(BigNumericValue, BigNumericValue, BigNumericValue*, absl::Status*)>
void Operator(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  EachRow(info, input, output, [](const Arguments& arguments) -> absl::StatusOr<std::string> {
    const auto in1 = Units(arguments, 0);
    if (!in1.ok()) {
      return in1.status();
    }
    const auto in2 = Units(arguments, 1);
    if (!in2.ok()) {
      return in2.status();
    }
    BigNumericValue out;
    absl::Status error;
    if (!kFunction(*in1, *in2, &out, &error)) {
      return error;
    }
    return BigNumericUnits(out);
  });
}

void Negate(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  EachRow(info, input, output, [](const Arguments& arguments) -> absl::StatusOr<std::string> {
    const auto in = Units(arguments, 0);
    if (!in.ok()) {
      return in.status();
    }
    BigNumericValue out;
    absl::Status error;
    if (!fn::UnaryMinus(*in, &out, &error)) {
      return error;
    }
    return BigNumericUnits(out);
  });
}

// The units of a BIGNUM that DuckDB's sum() computed exactly, which fails as SUM does when they
// are out of range.
void Sum(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  EachRow(info, input, output, [](const Arguments& arguments) -> absl::StatusOr<std::string> {
    if (!Units(arguments, 0).ok()) {
      return absl::OutOfRangeError("BIGNUMERIC overflow: SUM");
    }
    return arguments.String(0);
  });
}

void FromString(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  EachRow(info, input, output, [](const Arguments& arguments) {
    BigNumericValue out;
    absl::Status error;
    const bool ok = fn::StringToNumeric(arguments.String(0), &out, &error);
    return OrNull(ok, BigNumericUnits(out), error, arguments.Bool(1));
  });
}

void FromDouble(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  EachRow(info, input, output, [](const Arguments& arguments) {
    BigNumericValue out;
    absl::Status error;
    const bool ok = fn::Convert(arguments.Double(0), &out, &error);
    return OrNull(ok, BigNumericUnits(out), error, arguments.Bool(1));
  });
}

void ToString(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  EachRow(info, input, output, [](const Arguments& arguments) -> absl::StatusOr<std::string> {
    const auto in = Units(arguments, 0);
    if (!in.ok()) {
      return in.status();
    }
    std::string out;
    absl::Status error;
    if (!fn::NumericToString(*in, &out, &error, /*canonicalize_zero=*/true)) {
      return error;
    }
    return out;
  });
}

// The conversion of argument 0, a BIGNUMERIC, to `T`.
template <typename T>
absl::StatusOr<std::optional<T>> Convert(const Arguments& arguments) {
  const auto in = Units(arguments, 0);
  if (!in.ok()) {
    return in.status();
  }
  T out{};
  absl::Status error;
  const bool ok = fn::Convert(*in, &out, &error);
  return OrNull(ok, out, error, arguments.Bool(1));
}

template <typename T>
void ToNumber(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  EachRow(info, input, output, Convert<T>);
}

// A NUMERIC as its text, which the translator casts to DECIMAL(38, 9).
void ToNumeric(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  EachRow(info, input, output,
          [](const Arguments& arguments) -> absl::StatusOr<std::optional<std::string>> {
            const auto out = Convert<googlesql::NumericValue>(arguments);
            if (!out.ok()) {
              return out.status();
            }
            if (!out->has_value()) {
              return std::nullopt;
            }
            return (*out)->ToString();
          });
}

}  // namespace

void RegisterBigNumericFunctions(duckdb_connection connection) {
  Register(connection, "bq_bignumeric_add", {kVarchar, kVarchar}, kVarchar,
           Operator<fn::Add<BigNumericValue>>);
  Register(connection, "bq_bignumeric_subtract", {kVarchar, kVarchar}, kVarchar,
           Operator<fn::Subtract<BigNumericValue>>);
  Register(connection, "bq_bignumeric_negate", {kVarchar}, kVarchar, Negate);
  Register(connection, "bq_bignumeric_sum", {kVarchar}, kVarchar, Sum);
  Register(connection, "bq_bignumeric_from_string", {kVarchar, kBoolean}, kVarchar, FromString);
  Register(connection, "bq_bignumeric_from_double", {kDouble, kBoolean}, kVarchar, FromDouble);
  Register(connection, "bq_bignumeric_to_string", {kVarchar}, kVarchar, ToString);
  Register(connection, "bq_bignumeric_to_numeric", {kVarchar, kBoolean}, kVarchar, ToNumeric);
  Register(connection, "bq_bignumeric_to_int64", {kVarchar, kBoolean}, kBigint, ToNumber<int64_t>);
  Register(connection, "bq_bignumeric_to_double", {kVarchar, kBoolean}, kDouble, ToNumber<double>);
}

}  // namespace bigquery_emulator_duckdb::backend_functions
