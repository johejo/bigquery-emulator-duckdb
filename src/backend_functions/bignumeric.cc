#include "src/bignumeric.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
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
#include "src/backend_error.h"
#include "src/duckdb_handle.h"

// BIGNUMERIC arithmetic and conversions. The translator passes a BIGNUM to these functions, and
// takes one back, as the VARCHAR of its units; see src/bignumeric.h. A conversion's last argument
// is true under SAFE_CAST, which makes its errors NULL.

#include "src/backend_functions/apply.h"
#include "src/backend_functions/bignumeric.h"
#include "src/backend_functions/register.h"
#include "src/backend_functions/scalar.h"

namespace bigquery_emulator_duckdb::backend_functions {
namespace {

namespace fn = googlesql::functions;
using googlesql::BigNumericValue;

// The BIGNUMERIC of argument `column`, which holds the units of a BIGNUM.
absl::StatusOr<BigNumericValue> Units(const Arguments& arguments, idx_t column) {
  return BigNumericFromUnits(arguments.String(column));
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

// GENERATE_ARRAY(start, end, step).
absl::Status GenerateArray(BigNumericValue start, BigNumericValue end, BigNumericValue step,
                           std::vector<BigNumericValue>* out) {
  return fn::GenerateArrayHelper<fn::ArrayGenTrait<BigNumericValue, BigNumericValue>,
                                 kMaxGeneratedArraySize>(start, end, step, out);
}

bool FromString(std::string_view text, BigNumericValue* out, absl::Status* error) {
  return fn::StringToNumeric(text, out, error);
}

bool FromDouble(double value, BigNumericValue* out, absl::Status* error) {
  return fn::Convert(value, out, error);
}

bool ToString(BigNumericValue value, std::string* out, absl::Status* error) {
  return fn::NumericToString(value, out, error, /*canonicalize_zero=*/true);
}

// A NUMERIC as its text, which the translator casts to DECIMAL(38, 9).
bool ToNumeric(BigNumericValue value, std::string* out, absl::Status* error) {
  googlesql::NumericValue numeric;
  if (!fn::Convert(value, &numeric, error)) {
    return false;
  }
  *out = numeric.ToString();
  return true;
}

// The precision and scale of a BIGNUMERIC(P, S) column, as GoogleSQL's reference implementation
// applies them: a value is rounded half away from zero to the scale, then must have no more
// digits than the precision.
struct PrecisionScale {
  int64_t precision;
  int64_t scale;

  [[nodiscard]] absl::StatusOr<BigNumericValue> Apply(BigNumericValue value) const {
    const auto rounded = value.Round(scale);
    if (!rounded.ok()) {
      return rounded.status();
    }
    std::string nines(precision, '9');
    if (scale > 0) {
      nines.insert(nines.size() - scale, ".");
    }
    const auto upper = BigNumericValue::FromString(nines);
    if (!upper.ok()) {
      return upper.status();
    }
    if (*rounded <= *upper && *rounded >= upper->Negate().value()) {
      return *rounded;
    }
    const std::string type = scale == 0 ? std::format("BIGNUMERIC({})", precision)
                                        : std::format("BIGNUMERIC({}, {})", precision, scale);
    return absl::OutOfRangeError(std::format(
        "{} has precision {} and scale {} but got a value that is not in range of [-{}, {}]", type,
        precision, scale, upper->ToString(), upper->ToString()));
  }
};

// The cast to a stored BIGNUMERIC type from a BIGNUM or, with `kUnits`, from the VARCHAR
// of its units, which loads write. Extra parameters enforce a declared precision and scale.
template <bool kUnits>
bool CastToStorage(duckdb_function_info info, idx_t count, duckdb_vector input,
                   duckdb_vector output) {
  const auto* parameters =
      static_cast<const PrecisionScale*>(duckdb_cast_function_get_extra_info(info));
  const bool try_cast = duckdb_cast_function_get_cast_mode(info) == DUCKDB_CAST_TRY;
  uint64_t* validity = duckdb_vector_get_validity(input);
  duckdb_vector_ensure_validity_writable(output);
  for (idx_t row = 0; row < count; ++row) {
    if (validity != nullptr && !duckdb_validity_row_is_valid(validity, row)) {
      duckdb_validity_set_row_invalid(duckdb_vector_get_validity(output), row);
      continue;
    }
    const std::string stored = VectorString(input, row);
    auto value = kUnits ? BigNumericFromUnits(stored) : BigNumericFromBignum(stored);
    if (value.ok() && parameters != nullptr) {
      value = parameters->Apply(*value);
    }
    if (!value.ok()) {
      const std::string message(value.status().message());
      if (!try_cast) {
        duckdb_cast_function_set_error(info, message.c_str());
        return false;
      }
      duckdb_cast_function_set_row_error(info, message.c_str(), row, output);
      continue;
    }
    SetResult(output, row, BigNumericBignum(*value));
  }
  return true;
}

// The cast from a BIGNUMERIC(P, S) column's type to BIGNUM, whose values it keeps.
bool CastFromStorage(duckdb_function_info /*info*/, idx_t count, duckdb_vector input,
                     duckdb_vector output) {
  uint64_t* validity = duckdb_vector_get_validity(input);
  duckdb_vector_ensure_validity_writable(output);
  for (idx_t row = 0; row < count; ++row) {
    if (validity != nullptr && !duckdb_validity_row_is_valid(validity, row)) {
      duckdb_validity_set_row_invalid(duckdb_vector_get_validity(output), row);
      continue;
    }
    SetResult(output, row, VectorString(input, row));
  }
  return true;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): a cast's source and target types.
void RegisterCast(duckdb_connection connection, duckdb_logical_type source,
                  duckdb_logical_type target, duckdb_cast_function_t function,
                  std::optional<PrecisionScale> parameters, int64_t implicit_cost) {
  Handle<duckdb_cast_function, duckdb_destroy_cast_function> const cast(
      duckdb_create_cast_function());
  duckdb_cast_function_set_source_type(cast.get(), source);
  duckdb_cast_function_set_target_type(cast.get(), target);
  duckdb_cast_function_set_function(cast.get(), function);
  if (parameters) {
    duckdb_cast_function_set_extra_info(
        cast.get(), new PrecisionScale(*parameters),
        [](void* extra) { delete static_cast<PrecisionScale*>(extra); });
  }
  if (implicit_cost >= 0) {
    duckdb_cast_function_set_implicit_cast_cost(cast.get(), implicit_cost);
  }
  if (duckdb_register_cast_function(connection, cast.get()) == DuckDBError) {
    throw BackendError("DuckDB failed to register a cast for stored BIGNUMERIC");
  }
}

// A BIGNUMERIC(P, S) column keeps BIGNUMs under a type of its own, which DuckDB casts every value
// written to the column to, as it does to the DECIMAL(P, S) of a NUMERIC(P, S) column. A
// BIGNUMERIC(P, S) reads as a BIGNUM wherever DuckDB expects one; the translator casts columns
// to BIGNUM, since DuckDB has no cast between two of these types.
void RegisterBigNumericTypes(duckdb_connection connection) {
  LogicalType const bignum(duckdb_create_logical_type(DUCKDB_TYPE_BIGNUM));
  LogicalType const varchar(duckdb_create_logical_type(kVarchar));
  LogicalType const stored(duckdb_create_logical_type(DUCKDB_TYPE_BIGNUM));
  duckdb_logical_type_set_alias(stored.get(), kBigNumericStorageType);
  if (duckdb_register_logical_type(connection, stored.get(), nullptr) == DuckDBError) {
    throw BackendError("DuckDB failed to register BIGNUMERIC storage type");
  }
  RegisterCast(connection, bignum.get(), stored.get(), CastToStorage<false>, std::nullopt, -1);
  RegisterCast(connection, varchar.get(), stored.get(), CastToStorage<true>, std::nullopt, -1);
  RegisterCast(connection, stored.get(), bignum.get(), CastFromStorage, std::nullopt, 0);
  for (int64_t scale = 0; scale <= BigNumericValue::kMaxFractionalDigits; ++scale) {
    // DDL allows from 1, or the scale if greater, to 38 more digits than the scale.
    for (int64_t precision = std::max<int64_t>(1, scale); precision <= 38 + scale; ++precision) {
      LogicalType const type(duckdb_create_logical_type(DUCKDB_TYPE_BIGNUM));
      duckdb_logical_type_set_alias(type.get(), BigNumericTypeName(precision, scale).c_str());
      if (duckdb_register_logical_type(connection, type.get(), nullptr) == DuckDBError) {
        throw BackendError("DuckDB failed to register BIGNUMERIC(P, S)");
      }
      const PrecisionScale parameters{.precision = precision, .scale = scale};
      RegisterCast(connection, bignum.get(), type.get(), CastToStorage<false>, parameters, -1);
      RegisterCast(connection, varchar.get(), type.get(), CastToStorage<true>, parameters, -1);
      RegisterCast(connection, type.get(), bignum.get(), CastFromStorage, std::nullopt, 0);
    }
  }
}

}  // namespace

void RegisterBigNumericFunctions(duckdb_connection connection) {
  Register(connection, "bq_parse_bignumeric", {kVarchar}, kVarchar, Apply<fn::ParseBigNumeric>);
  Register(connection, "bq_bignumeric_add", {kVarchar, kVarchar}, kVarchar,
           Apply<fn::Add<BigNumericValue>>);
  Register(connection, "bq_bignumeric_subtract", {kVarchar, kVarchar}, kVarchar,
           Apply<fn::Subtract<BigNumericValue>>);
  Register(connection, "bq_bignumeric_multiply", {kVarchar, kVarchar}, kVarchar,
           Apply<fn::Multiply<BigNumericValue>>);
  Register(connection, "bq_bignumeric_divide", {kVarchar, kVarchar}, kVarchar,
           Apply<fn::Divide<BigNumericValue>>);
  Register(connection, "bq_bignumeric_safe_divide", {kVarchar, kVarchar}, kVarchar,
           Apply<fn::Divide<BigNumericValue>, Errors::kNull>);
  Register(connection, "bq_bignumeric_div", {kVarchar, kVarchar}, kVarchar,
           Apply<fn::DivideToIntegralValue<BigNumericValue>>);
  Register(connection, "bq_bignumeric_mod", {kVarchar, kVarchar}, kVarchar,
           Apply<fn::Modulo<BigNumericValue>>);
  Register(connection, "bq_bignumeric_negate", {kVarchar}, kVarchar,
           Apply<fn::UnaryMinus<BigNumericValue, BigNumericValue>>);
  Register(connection, "bq_bignumeric_abs", {kVarchar}, kVarchar, Apply<fn::Abs<BigNumericValue>>);
  Register(connection, "bq_bignumeric_sign", {kVarchar}, kVarchar,
           Apply<fn::Sign<BigNumericValue>>);
  Register(connection, "bq_bignumeric_round", {kVarchar}, kVarchar,
           Apply<fn::Round<BigNumericValue>>);
  Register(connection, "bq_bignumeric_round_digits", {kVarchar, kBigint}, kVarchar,
           Apply<fn::RoundDecimal<BigNumericValue>>);
  Register(connection, "bq_bignumeric_round_half_even", {kVarchar, kBigint}, kVarchar,
           Apply<RoundWithMode<fn::ROUND_HALF_EVEN>>);
  Register(connection, "bq_bignumeric_trunc", {kVarchar}, kVarchar,
           Apply<fn::Trunc<BigNumericValue>>);
  Register(connection, "bq_bignumeric_trunc_digits", {kVarchar, kBigint}, kVarchar,
           Apply<fn::TruncDecimal<BigNumericValue>>);
  Register(connection, "bq_bignumeric_ceil", {kVarchar}, kVarchar,
           Apply<fn::Ceil<BigNumericValue>>);
  Register(connection, "bq_bignumeric_floor", {kVarchar}, kVarchar,
           Apply<fn::Floor<BigNumericValue>>);
  Register(connection, "bq_bignumeric_sqrt", {kVarchar}, kVarchar,
           Apply<fn::Sqrt<BigNumericValue>>);
  Register(connection, "bq_bignumeric_cbrt", {kVarchar}, kVarchar,
           Apply<fn::Cbrt<BigNumericValue>>);
  Register(connection, "bq_bignumeric_exp", {kVarchar}, kVarchar, Apply<fn::Exp<BigNumericValue>>);
  Register(connection, "bq_bignumeric_ln", {kVarchar}, kVarchar,
           Apply<fn::NaturalLogarithm<BigNumericValue>>);
  Register(connection, "bq_bignumeric_log10", {kVarchar}, kVarchar,
           Apply<fn::DecimalLogarithm<BigNumericValue>>);
  Register(connection, "bq_bignumeric_pow", {kVarchar, kVarchar}, kVarchar,
           Apply<fn::Pow<BigNumericValue>>);
  Register(connection, "bq_bignumeric_log", {kVarchar, kVarchar}, kVarchar,
           Apply<fn::Logarithm<BigNumericValue>>);
  Register(connection, "bq_bignumeric_sum", {kVarchar}, kVarchar, Sum);
  LogicalType const varchar(duckdb_create_logical_type(kVarchar));
  LogicalType const list(duckdb_create_list_type(varchar.get()));
  LogicalType const float64(duckdb_create_logical_type(kDouble));
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
           Apply<GenerateArray>);
  Register(connection, "bq_bignumeric_from_string", {kVarchar, kBoolean}, kVarchar,
           Apply<FromString, Errors::kNullUnderSafeCast>);
  Register(connection, "bq_bignumeric_from_double", {kDouble, kBoolean}, kVarchar,
           Apply<FromDouble, Errors::kNullUnderSafeCast>);
  Register(connection, "bq_bignumeric_to_string", {kVarchar}, kVarchar, Apply<ToString>);
  Register(connection, "bq_bignumeric_from_decimal_bytes", {kBlob, kBigint}, kVarchar,
           Apply<BigNumericFromDecimalBytes>);
  Register(connection, "bq_bignumeric_to_decimal_bytes", {kVarchar}, kBlob,
           Apply<BigNumericDecimalBytes>);
  Register(connection, "bq_bignumeric_to_numeric", {kVarchar, kBoolean}, kVarchar,
           Apply<ToNumeric, Errors::kNullUnderSafeCast>);
  Register(connection, "bq_bignumeric_to_int64", {kVarchar, kBoolean}, kBigint,
           Apply<fn::Convert<BigNumericValue, int64_t>, Errors::kNullUnderSafeCast>);
  Register(connection, "bq_bignumeric_to_double", {kVarchar, kBoolean}, kDouble,
           Apply<fn::Convert<BigNumericValue, double>, Errors::kNullUnderSafeCast>);
  RegisterBigNumericTypes(connection);
}

}  // namespace bigquery_emulator_duckdb::backend_functions
