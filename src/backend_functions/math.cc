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
#include "googlesql/public/functions/distance.h"
#include "googlesql/public/functions/generate_array.h"
#include "googlesql/public/functions/numeric.h"
#include "googlesql/public/numeric_value.h"
#include "googlesql/public/types/type_factory.h"
#include "googlesql/public/value.h"
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

// Read only the vector types the distance signatures resolve: DOUBLE arrays and
// sparse arrays of (INT64 or STRING, DOUBLE). Preserve nulls for GoogleSQL to validate.
absl::StatusOr<googlesql::Value> DistanceElement(duckdb_vector vector, idx_t row,
                                                 const googlesql::Type* type) {
  using GValue = googlesql::Value;
  uint64_t* validity = duckdb_vector_get_validity(vector);
  if (validity != nullptr && !duckdb_validity_row_is_valid(validity, row)) {
    return GValue::Null(type);
  }
  if (type->IsDouble()) {
    return GValue::Double(VectorElement<double>(vector, row));
  }
  if (type->IsInt64()) {
    return GValue::Int64(VectorElement<int64_t>(vector, row));
  }
  if (type->IsString()) {
    return GValue::String(VectorString(vector, row));
  }
  const auto* structure = type->AsStruct();
  std::vector<GValue> fields;
  for (idx_t i = 0; i < 2; ++i) {
    auto field = DistanceElement(duckdb_struct_vector_get_child(vector, i), row,
                                 structure->field(static_cast<int>(i)).type);
    if (!field.ok()) {
      return field.status();
    }
    fields.push_back(*std::move(field));
  }
  return GValue::MakeStruct(structure, std::move(fields));
}

absl::StatusOr<googlesql::Value> DistanceArray(const Arguments& arguments, idx_t column,
                                               const googlesql::ArrayType* type) {
  duckdb_vector vector = arguments.Vector(column);
  const auto entry = VectorElement<duckdb_list_entry>(vector, arguments.Row());
  duckdb_vector child = duckdb_list_vector_get_child(vector);
  std::vector<googlesql::Value> elements;
  elements.reserve(entry.length);
  for (idx_t i = 0; i < entry.length; ++i) {
    auto element = DistanceElement(child, entry.offset + i, type->element_type());
    if (!element.ok()) {
      return element.status();
    }
    elements.push_back(*std::move(element));
  }
  return googlesql::Value::MakeArray(type, std::move(elements));
}

// ANY preserves callers' struct field names; distance semantics use field positions.
// GoogleSQL checks intermediate arithmetic as well as lengths, duplicate dimensions,
// null elements/fields and cosine's zero vectors. DuckDB's distance functions instead
// return infinities on finite overflow and clamp cosine results.
template <bool kCosine>
void Distance(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  namespace fn = googlesql::functions;
  namespace types = googlesql::types;
  using Function = absl::StatusOr<googlesql::Value> (*)(googlesql::Value, googlesql::Value);
  Function function = kCosine ? fn::CosineDistanceDense : fn::EuclideanDistanceDense;
  googlesql::TypeFactory factory;
  const googlesql::ArrayType* array = types::DoubleArrayType();
  duckdb_vector vector = duckdb_data_chunk_get_vector(input, 0);
  LogicalType const list_type(duckdb_vector_get_column_type(vector));
  LogicalType const element(duckdb_list_type_child_type(list_type.get()));
  if (duckdb_get_type_id(element.get()) == DUCKDB_TYPE_STRUCT) {
    LogicalType const key(duckdb_struct_type_child_type(element.get(), 0));
    const bool string_key = duckdb_get_type_id(key.get()) == kVarchar;
    const googlesql::StructType* structure = nullptr;
    absl::Status const status = factory.MakeStructType(
        {{"", string_key ? types::StringType() : types::Int64Type()}, {"", types::DoubleType()}},
        &structure);
    if (!status.ok()) {
      duckdb_scalar_function_set_error(info, std::string(status.message()).c_str());
      return;
    }
    const auto sparse_array = factory.MakeArrayType(structure);
    if (!sparse_array.ok()) {
      duckdb_scalar_function_set_error(info, std::string(sparse_array.status().message()).c_str());
      return;
    }
    array = *sparse_array;
    function =
        string_key
            ? (kCosine ? fn::CosineDistanceSparseStringKey : fn::EuclideanDistanceSparseStringKey)
            : (kCosine ? fn::CosineDistanceSparseInt64Key : fn::EuclideanDistanceSparseInt64Key);
  }
  EachRow(
      info, input, output, [array, function](const Arguments& arguments) -> absl::StatusOr<double> {
        auto first = DistanceArray(arguments, 0, array);
        if (!first.ok()) {
          return first.status();
        }
        auto second = DistanceArray(arguments, 1, array);
        if (!second.ok()) {
          return second.status();
        }
        const auto result = function(*std::move(first), *std::move(second));
        if (!result.ok()) {
          if constexpr (kCosine) {
            if (result.status().message() == "Cannot compute cosine distance against zero vector") {
              return absl::OutOfRangeError(
                  "Cannot compute cosine distance against zero vector. Error in COSINE_DISTANCE "
                  "expression");
            }
          }
          return result.status();
        }
        return result->double_value();
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
                bounds.at(column) = arguments.Double(column);
              } else {
                const auto bound = Decimal<T>::From(arguments.Decimal(column));
                if (!bound.ok()) {
                  return bound.status();
                }
                bounds.at(column) = *bound;
              }
            }
            std::vector<T> values;
            namespace fn = googlesql::functions;
            if (absl::Status status =
                    fn::GenerateArrayHelper<fn::ArrayGenTrait<T, T>, kMaxGeneratedArraySize>(
                        bounds.at(0), bounds.at(1), bounds.at(2), &values);
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
  LogicalType const type(duckdb_create_decimal_type(38, Decimal<T>::kScale));
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
  LogicalType const list(duckdb_create_list_type(type.get()));
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
  LogicalType const float64(duckdb_create_logical_type(kDouble));
  LogicalType const list(duckdb_create_list_type(float64.get()));
  Register(connection, "bq_generate_array", {kDouble, kDouble, kDouble}, list.get(),
           GenerateArray<double>);
  RegisterDecimalMath<googlesql::NumericValue>(connection, "_numeric");
  Register(connection, "bq_cosine_distance", {kAny, kAny}, kDouble, Distance<true>);
  Register(connection, "bq_euclidean_distance", {kAny, kAny}, kDouble, Distance<false>);
}

}  // namespace bigquery_emulator_duckdb::backend_functions
