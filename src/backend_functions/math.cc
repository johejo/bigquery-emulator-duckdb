#include "googlesql/public/functions/math.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "duckdb.h"
#include "googlesql/public/functions/arithmetics.h"
#include "googlesql/public/functions/generate_array.h"
#include "googlesql/public/functions/numeric.h"
#include "googlesql/public/numeric_value.h"
#include "src/backend_functions/math.h"
#include "src/backend_functions/register.h"
#include "src/backend_functions/scalar.h"
#include "src/duckdb_handle.h"

namespace bigquery_emulator_duckdb::backend_functions {
namespace {

// A FLOAT64 function of one argument, such as SQRT, which fails where GoogleSQL's does.
template <bool (*kFunction)(double, double*, absl::Status*)>
void Math(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  EachRow(info, input, output, [](const Arguments& arguments) {
    double out = 0;
    absl::Status error;
    const bool ok = kFunction(arguments.Double(0), &out, &error);
    return ToStatusOr(ok, out, error);
  });
}

// A FLOAT64 function of two arguments, such as POW.
template <bool (*kFunction)(double, double, double*, absl::Status*)>
void Math2(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  EachRow(info, input, output, [](const Arguments& arguments) {
    double out = 0;
    absl::Status error;
    const bool ok = kFunction(arguments.Double(0), arguments.Double(1), &out, &error);
    return ToStatusOr(ok, out, error);
  });
}

// DuckDB keeps NUMERIC as DECIMAL(38, 9), whose units are the packed integer of NumericValue.
template <typename T>
struct Decimal;

template <>
struct Decimal<googlesql::NumericValue> {
  static constexpr uint8_t kScale = 9;

  static absl::StatusOr<googlesql::NumericValue> From(__int128 units) {
    return googlesql::NumericValue::FromPackedInt(units);
  }

  static absl::StatusOr<__int128> To(googlesql::NumericValue value) {
    return value.as_packed_int();
  }
};

// A NUMERIC function of one argument, such as SQRT, which fails where GoogleSQL's does.
template <typename T, bool (*kFunction)(T, T*, absl::Status*)>
void DecimalMath(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  EachRow(info, input, output, [](const Arguments& arguments) -> absl::StatusOr<__int128> {
    const auto in = Decimal<T>::From(arguments.Decimal(0));
    if (!in.ok()) {
      return in.status();
    }
    T out;
    absl::Status error;
    if (!kFunction(*in, &out, &error)) {
      return error;
    }
    return Decimal<T>::To(out);
  });
}

// A NUMERIC function of two arguments, such as POW.
template <typename T, bool (*kFunction)(T, T, T*, absl::Status*)>
void DecimalMath2(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  EachRow(info, input, output, [](const Arguments& arguments) -> absl::StatusOr<__int128> {
    const auto in1 = Decimal<T>::From(arguments.Decimal(0));
    if (!in1.ok()) {
      return in1.status();
    }
    const auto in2 = Decimal<T>::From(arguments.Decimal(1));
    if (!in2.ok()) {
      return in2.status();
    }
    T out;
    absl::Status error;
    if (!kFunction(*in1, *in2, &out, &error)) {
      return error;
    }
    return Decimal<T>::To(out);
  });
}

// DuckDB has no NUMERIC or FLOAT64 generate_series overload. GoogleSQL supplies the
// generation and validation, with BigQuery's documented 1,048,576-element limit.
constexpr int kMaxGeneratedArraySize = 1048576;
template <typename T>
void GenerateArray(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  using Element = std::conditional_t<std::is_same_v<T, double>, double, __int128>;
  EachRow(info, input, output,
          [](const Arguments& arguments) -> absl::StatusOr<std::vector<Element>> {
            std::array<T, 3> bounds{};
            for (idx_t column = 0; column < bounds.size(); ++column) {
              if constexpr (std::is_same_v<T, double>) {
                bounds[column] = arguments.Double(column);
              } else {
                const auto bound = Decimal<T>::From(arguments.Decimal(column));
                if (!bound.ok()) {
                  return bound.status();
                }
                bounds[column] = *bound;
              }
            }
            std::vector<T> values;
            namespace fn = googlesql::functions;
            if (absl::Status status =
                    fn::GenerateArrayHelper<fn::ArrayGenTrait<T, T>, kMaxGeneratedArraySize>(
                        bounds[0], bounds[1], bounds[2], &values);
                !status.ok()) {
              return status;
            }
            std::vector<Element> result(values.size());
            std::ranges::transform(values, result.begin(), [](const T& value) -> Element {
              if constexpr (std::is_same_v<T, double>) {
                return value;
              } else {
                return value.as_packed_int();
              }
            });
            return result;
          });
}

void ParseNumeric(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  EachRow(info, input, output, [](const Arguments& arguments) -> absl::StatusOr<__int128> {
    googlesql::NumericValue out;
    absl::Status error;
    if (!googlesql::functions::ParseNumeric(arguments.String(0), &out, &error)) {
      return error;
    }
    return out.as_packed_int();
  });
}

// Registers the NUMERIC functions, named as the FLOAT64 ones plus `suffix`.
template <typename T>
void RegisterDecimalMath(duckdb_connection connection, const std::string& suffix) {
  namespace fn = googlesql::functions;
  LogicalType type(duckdb_create_decimal_type(38, Decimal<T>::kScale));
  for (const auto& [name, function] :
       std::initializer_list<std::pair<const char*, duckdb_scalar_function_t>>{
           {"bq_sqrt", DecimalMath<T, fn::Sqrt<T>>},
           {"bq_cbrt", DecimalMath<T, fn::Cbrt<T>>},
           {"bq_exp", DecimalMath<T, fn::Exp<T>>},
           {"bq_ln", DecimalMath<T, fn::NaturalLogarithm<T>>},
           {"bq_log10", DecimalMath<T, fn::DecimalLogarithm<T>>},
       }) {
    Register(connection, (name + suffix).c_str(), {type.get()}, type.get(), function);
  }
  LogicalType list(duckdb_create_list_type(type.get()));
  Register(connection, "bq_generate_array_numeric", {type.get(), type.get(), type.get()},
           list.get(), GenerateArray<T>);
  // Spelled out: a braced pair of pointers would also match vector<duckdb_type>'s iterator range
  // constructor.
  const std::vector<duckdb_logical_type> two = {type.get(), type.get()};
  Register(connection, ("bq_pow" + suffix).c_str(), two, type.get(), DecimalMath2<T, fn::Pow<T>>);
  Register(connection, "bq_div_numeric", two, type.get(),
           DecimalMath2<T, fn::DivideToIntegralValue<T>>);
  Register(connection, ("bq_log" + suffix).c_str(), two, type.get(),
           DecimalMath2<T, fn::Logarithm<T>>);
  Register(connection, "bq_parse_numeric", {kVarchar}, type.get(), ParseNumeric);
}

}  // namespace

void RegisterMathFunctions(duckdb_connection connection) {
  namespace fn = googlesql::functions;
  for (const auto& [name, function] :
       std::initializer_list<std::pair<const char*, duckdb_scalar_function_t>>{
           {"bq_sqrt", Math<fn::Sqrt<double>>},
           {"bq_cbrt", Math<fn::Cbrt<double>>},
           {"bq_exp", Math<fn::Exp<double>>},
           {"bq_ln", Math<fn::NaturalLogarithm<double>>},
           {"bq_log10", Math<fn::DecimalLogarithm<double>>},
           {"bq_sin", Math<fn::Sin<double>>},
           {"bq_cos", Math<fn::Cos<double>>},
           {"bq_tan", Math<fn::Tan<double>>},
           {"bq_asin", Math<fn::Asin<double>>},
           {"bq_acos", Math<fn::Acos<double>>},
           {"bq_sinh", Math<fn::Sinh<double>>},
           {"bq_cosh", Math<fn::Cosh<double>>},
           {"bq_acosh", Math<fn::Acosh<double>>},
           {"bq_atanh", Math<fn::Atanh<double>>},
           {"bq_csc", Math<fn::Csc<double>>},
           {"bq_sec", Math<fn::Sec<double>>},
           {"bq_cot", Math<fn::Cot<double>>},
           {"bq_csch", Math<fn::Csch<double>>},
           {"bq_sech", Math<fn::Sech<double>>},
           {"bq_coth", Math<fn::Coth<double>>},
       }) {
    Register(connection, name, {kDouble}, kDouble, function);
  }
  Register(connection, "bq_pow", {kDouble, kDouble}, kDouble, Math2<fn::Pow<double>>);
  Register(connection, "bq_log", {kDouble, kDouble}, kDouble, Math2<fn::Logarithm<double>>);
  LogicalType float64(duckdb_create_logical_type(kDouble));
  LogicalType list(duckdb_create_list_type(float64.get()));
  Register(connection, "bq_generate_array", {kDouble, kDouble, kDouble}, list.get(),
           GenerateArray<double>);
  RegisterDecimalMath<googlesql::NumericValue>(connection, "_numeric");
}

}  // namespace bigquery_emulator_duckdb::backend_functions
