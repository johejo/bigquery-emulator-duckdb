#include "src/bignumeric.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "duckdb.h"
#include "googlesql/public/functions/arithmetics.h"
#include "googlesql/public/functions/convert.h"
#include "googlesql/public/functions/convert_string.h"
#include "googlesql/public/functions/generate_array.h"
#include "googlesql/public/functions/math.h"
#include "googlesql/public/functions/numeric.h"
#include "googlesql/public/functions/rounding_mode.pb.h"
#include "googlesql/public/numeric_value.h"
#include "src/backend_functions/internal.h"
#include "src/duckdb_handle.h"

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

// A BIGNUMERIC function of two BIGNUMERIC arguments, such as *. Unless `kSafe`, its errors
// fail the query rather than being NULL, as SAFE_DIVIDE's are.
template <bool (*kFunction)(BigNumericValue, BigNumericValue, BigNumericValue*, absl::Status*),
          bool kSafe = false>
void Operator(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  EachRow(info, input, output,
          [](const Arguments& arguments) -> absl::StatusOr<std::optional<std::string>> {
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
            const bool ok = kFunction(*in1, *in2, &out, &error);
            return OrNull(ok, BigNumericUnits(out), error, kSafe);
          });
}

// A BIGNUMERIC function of one BIGNUMERIC argument, such as ABS.
template <bool (*kFunction)(BigNumericValue, BigNumericValue*, absl::Status*)>
void Unary(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  EachRow(info, input, output, [](const Arguments& arguments) -> absl::StatusOr<std::string> {
    const auto in = Units(arguments, 0);
    if (!in.ok()) {
      return in.status();
    }
    BigNumericValue out;
    absl::Status error;
    if (!kFunction(*in, &out, &error)) {
      return error;
    }
    return BigNumericUnits(out);
  });
}

// A BIGNUMERIC function of a BIGNUMERIC and a number of digits, such as ROUND(x, 2).
template <bool (*kFunction)(BigNumericValue, int64_t, BigNumericValue*, absl::Status*)>
void WithDigits(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  EachRow(info, input, output, [](const Arguments& arguments) -> absl::StatusOr<std::string> {
    const auto in = Units(arguments, 0);
    if (!in.ok()) {
      return in.status();
    }
    BigNumericValue out;
    absl::Status error;
    if (!kFunction(*in, arguments.Int(1), &out, &error)) {
      return error;
    }
    return BigNumericUnits(out);
  });
}

// ROUND with a rounding mode.
template <fn::RoundingMode kMode>
bool RoundWithMode(BigNumericValue in, int64_t digits, BigNumericValue* out, absl::Status* error) {
  return fn::RoundDecimalWithRoundingMode(in, digits, kMode, out, error);
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

// An aggregate function of BIGNUMERIC takes the units of its non-NULL values, which the translator
// collects with list(). CORR and the COVAR functions take the units of each pair joined by a comma.
using Pair = std::pair<BigNumericValue, BigNumericValue>;

absl::StatusOr<std::vector<BigNumericValue>> Collected(const Arguments& arguments) {
  std::vector<BigNumericValue> values;
  for (const std::string& units : arguments.Strings(0)) {
    const auto value = BigNumericFromUnits(units);
    if (!value.ok()) {
      return value.status();
    }
    values.push_back(*value);
  }
  return values;
}

absl::StatusOr<std::vector<Pair>> CollectedPairs(const Arguments& arguments) {
  std::vector<Pair> pairs;
  for (const std::string& units : arguments.Strings(0)) {
    const std::size_t comma = units.find(',');
    if (comma == std::string::npos) {
      return absl::InternalError("Invalid BIGNUMERIC pair: " + units);
    }
    const auto x = BigNumericFromUnits(std::string_view(units).substr(0, comma));
    if (!x.ok()) {
      return x.status();
    }
    const auto y = BigNumericFromUnits(std::string_view(units).substr(comma + 1));
    if (!y.ok()) {
      return y.status();
    }
    pairs.emplace_back(*x, *y);
  }
  return pairs;
}

// AVG, rounded half away from zero, which is in range even where SUM overflows.
void Average(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  EachRow(info, input, output,
          [](const Arguments& arguments) -> absl::StatusOr<std::optional<std::string>> {
            const auto values = Collected(arguments);
            if (!values.ok()) {
              return values.status();
            }
            if (values->empty()) {
              return std::nullopt;
            }
            BigNumericValue::SumAggregator sum;
            for (const BigNumericValue& value : *values) {
              sum.Add(value);
            }
            const auto average = sum.GetAverage(values->size());
            if (!average.ok()) {
              return average.status();
            }
            return BigNumericUnits(*average);
          });
}

// VAR_POP and VAR_SAMP, or with `kStdDev` STDDEV_POP and STDDEV_SAMP, as FLOAT64.
template <bool kStdDev, bool kSampling>
void Variance(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  EachRow(info, input, output,
          [](const Arguments& arguments) -> absl::StatusOr<std::optional<double>> {
            const auto values = Collected(arguments);
            if (!values.ok()) {
              return values.status();
            }
            BigNumericValue::VarianceAggregator variance;
            for (const BigNumericValue& value : *values) {
              variance.Add(value);
            }
            return kStdDev ? variance.GetStdDev(values->size(), kSampling)
                           : variance.GetVariance(values->size(), kSampling);
          });
}

// COVAR_POP and COVAR_SAMP, as FLOAT64.
template <bool kSampling>
void Covariance(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  EachRow(info, input, output,
          [](const Arguments& arguments) -> absl::StatusOr<std::optional<double>> {
            const auto pairs = CollectedPairs(arguments);
            if (!pairs.ok()) {
              return pairs.status();
            }
            BigNumericValue::CovarianceAggregator covariance;
            for (const auto& [x, y] : *pairs) {
              covariance.Add(x, y);
            }
            return covariance.GetCovariance(pairs->size(), kSampling);
          });
}

// CORR, as FLOAT64.
void Correlation(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  EachRow(info, input, output,
          [](const Arguments& arguments) -> absl::StatusOr<std::optional<double>> {
            const auto pairs = CollectedPairs(arguments);
            if (!pairs.ok()) {
              return pairs.status();
            }
            BigNumericValue::CorrelationAggregator correlation;
            for (const auto& [x, y] : *pairs) {
              correlation.Add(x, y);
            }
            return correlation.GetCorrelation(pairs->size());
          });
}

// GoogleSQL limits GENERATE_ARRAY to 16000 elements, where BigQuery generates far longer arrays,
// as DuckDB's generate_series() does for INT64.
constexpr int kMaxGeneratedArraySize = std::numeric_limits<int>::max();

// GENERATE_ARRAY(start, end, step), as the units of its elements.
void GenerateArray(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  EachRow(
      info, input, output,
      [](const Arguments& arguments) -> absl::StatusOr<std::vector<std::optional<std::string>>> {
        std::vector<BigNumericValue> bounds;
        for (idx_t column = 0; column < 3; ++column) {
          const auto bound = Units(arguments, column);
          if (!bound.ok()) {
            return bound.status();
          }
          bounds.push_back(*bound);
        }
        std::vector<BigNumericValue> values;
        if (absl::Status status =
                fn::GenerateArrayHelper<fn::ArrayGenTrait<BigNumericValue, BigNumericValue>,
                                        kMaxGeneratedArraySize>(bounds[0], bounds[1], bounds[2],
                                                                &values);
            !status.ok()) {
          return status;
        }
        std::vector<std::optional<std::string>> units(values.size());
        std::ranges::transform(values, units.begin(),
                               [](const BigNumericValue& value) { return BigNumericUnits(value); });
        return units;
      });
}

void Parse(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  EachRow(info, input, output, [](const Arguments& arguments) -> absl::StatusOr<std::string> {
    BigNumericValue out;
    absl::Status error;
    if (!fn::ParseBigNumeric(arguments.String(0), &out, &error)) {
      return error;
    }
    return BigNumericUnits(out);
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

// A Parquet DECIMAL, its bytes and scale, as the units of a BIGNUMERIC; see src/load.cc.
void FromDecimalBytes(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  EachRow(info, input, output, [](const Arguments& arguments) -> absl::StatusOr<std::string> {
    const auto value = BigNumericFromDecimalBytes(arguments.String(0), arguments.Int(1));
    if (!value.ok()) {
      return value.status();
    }
    return BigNumericUnits(*value);
  });
}

// A BIGNUMERIC as the bytes of a Parquet DECIMAL(76, 38); see src/extract.cc.
void ToDecimalBytes(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  EachRow(info, input, output, [](const Arguments& arguments) -> absl::StatusOr<std::string> {
    const auto in = Units(arguments, 0);
    if (!in.ok()) {
      return in.status();
    }
    return BigNumericDecimalBytes(*in);
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
  Register(connection, "bq_parse_bignumeric", {kVarchar}, kVarchar, Parse);
  Register(connection, "bq_bignumeric_add", {kVarchar, kVarchar}, kVarchar,
           Operator<fn::Add<BigNumericValue>>);
  Register(connection, "bq_bignumeric_subtract", {kVarchar, kVarchar}, kVarchar,
           Operator<fn::Subtract<BigNumericValue>>);
  Register(connection, "bq_bignumeric_multiply", {kVarchar, kVarchar}, kVarchar,
           Operator<fn::Multiply<BigNumericValue>>);
  Register(connection, "bq_bignumeric_divide", {kVarchar, kVarchar}, kVarchar,
           Operator<fn::Divide<BigNumericValue>>);
  Register(connection, "bq_bignumeric_safe_divide", {kVarchar, kVarchar}, kVarchar,
           Operator<fn::Divide<BigNumericValue>, true>);
  Register(connection, "bq_bignumeric_div", {kVarchar, kVarchar}, kVarchar,
           Operator<fn::DivideToIntegralValue<BigNumericValue>>);
  Register(connection, "bq_bignumeric_mod", {kVarchar, kVarchar}, kVarchar,
           Operator<fn::Modulo<BigNumericValue>>);
  Register(connection, "bq_bignumeric_negate", {kVarchar}, kVarchar,
           Unary<fn::UnaryMinus<BigNumericValue, BigNumericValue>>);
  Register(connection, "bq_bignumeric_abs", {kVarchar}, kVarchar, Unary<fn::Abs<BigNumericValue>>);
  Register(connection, "bq_bignumeric_sign", {kVarchar}, kVarchar,
           Unary<fn::Sign<BigNumericValue>>);
  Register(connection, "bq_bignumeric_round", {kVarchar}, kVarchar,
           Unary<fn::Round<BigNumericValue>>);
  Register(connection, "bq_bignumeric_round_digits", {kVarchar, kBigint}, kVarchar,
           WithDigits<fn::RoundDecimal<BigNumericValue>>);
  Register(connection, "bq_bignumeric_round_half_even", {kVarchar, kBigint}, kVarchar,
           WithDigits<RoundWithMode<fn::ROUND_HALF_EVEN>>);
  Register(connection, "bq_bignumeric_trunc", {kVarchar}, kVarchar,
           Unary<fn::Trunc<BigNumericValue>>);
  Register(connection, "bq_bignumeric_trunc_digits", {kVarchar, kBigint}, kVarchar,
           WithDigits<fn::TruncDecimal<BigNumericValue>>);
  Register(connection, "bq_bignumeric_ceil", {kVarchar}, kVarchar,
           Unary<fn::Ceil<BigNumericValue>>);
  Register(connection, "bq_bignumeric_floor", {kVarchar}, kVarchar,
           Unary<fn::Floor<BigNumericValue>>);
  Register(connection, "bq_bignumeric_sqrt", {kVarchar}, kVarchar,
           Unary<fn::Sqrt<BigNumericValue>>);
  Register(connection, "bq_bignumeric_cbrt", {kVarchar}, kVarchar,
           Unary<fn::Cbrt<BigNumericValue>>);
  Register(connection, "bq_bignumeric_exp", {kVarchar}, kVarchar, Unary<fn::Exp<BigNumericValue>>);
  Register(connection, "bq_bignumeric_ln", {kVarchar}, kVarchar,
           Unary<fn::NaturalLogarithm<BigNumericValue>>);
  Register(connection, "bq_bignumeric_log10", {kVarchar}, kVarchar,
           Unary<fn::DecimalLogarithm<BigNumericValue>>);
  Register(connection, "bq_bignumeric_pow", {kVarchar, kVarchar}, kVarchar,
           Operator<fn::Pow<BigNumericValue>>);
  Register(connection, "bq_bignumeric_log", {kVarchar, kVarchar}, kVarchar,
           Operator<fn::Logarithm<BigNumericValue>>);
  Register(connection, "bq_bignumeric_sum", {kVarchar}, kVarchar, Sum);
  LogicalType varchar(duckdb_create_logical_type(kVarchar));
  LogicalType list(duckdb_create_list_type(varchar.get()));
  LogicalType float64(duckdb_create_logical_type(kDouble));
  Register(connection, "bq_bignumeric_avg", {list.get()}, varchar.get(), Average);
  for (const auto& [name, function] :
       std::initializer_list<std::pair<const char*, duckdb_scalar_function_t>>{
           {"bq_bignumeric_var_pop", Variance<false, false>},
           {"bq_bignumeric_var_samp", Variance<false, true>},
           {"bq_bignumeric_stddev_pop", Variance<true, false>},
           {"bq_bignumeric_stddev_samp", Variance<true, true>},
           {"bq_bignumeric_covar_pop", Covariance<false>},
           {"bq_bignumeric_covar_samp", Covariance<true>},
           {"bq_bignumeric_corr", Correlation},
       }) {
    Register(connection, name, {list.get()}, float64.get(), function);
  }
  Register(connection, "bq_bignumeric_generate_array", {kVarchar, kVarchar, kVarchar}, list.get(),
           GenerateArray);
  Register(connection, "bq_bignumeric_from_string", {kVarchar, kBoolean}, kVarchar, FromString);
  Register(connection, "bq_bignumeric_from_double", {kDouble, kBoolean}, kVarchar, FromDouble);
  Register(connection, "bq_bignumeric_to_string", {kVarchar}, kVarchar, ToString);
  Register(connection, "bq_bignumeric_from_decimal_bytes", {kBlob, kBigint}, kVarchar,
           FromDecimalBytes);
  Register(connection, "bq_bignumeric_to_decimal_bytes", {kVarchar}, kBlob, ToDecimalBytes);
  Register(connection, "bq_bignumeric_to_numeric", {kVarchar, kBoolean}, kVarchar, ToNumeric);
  Register(connection, "bq_bignumeric_to_int64", {kVarchar, kBoolean}, kBigint, ToNumber<int64_t>);
  Register(connection, "bq_bignumeric_to_double", {kVarchar, kBoolean}, kDouble, ToNumber<double>);
}

}  // namespace bigquery_emulator_duckdb::backend_functions
