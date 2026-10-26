#include "googlesql/public/functions/math.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <iterator>
#include <string>
#include <string_view>
#include <unordered_map>
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
#include "src/duckdb_functions/apply.h"
#include "src/duckdb_functions/register.h"
#include "src/duckdb_functions/scalar.h"
#include "src/duckdb_handle.h"
#include "src/function_catalog/rule_builders.h"
#include "src/function_catalog/rules.h"
#include "src/googlesql_functions_duckdb/math.h"
#include "src/googlesql_functions_duckdb/options.h"

namespace bigquery_emulator_duckdb::functions_duckdb {
namespace fn = googlesql::functions;
using namespace duckdb_functions;
using function_catalog::FunctionDefinition;
using function_catalog::Rule;
namespace {

// The public names, backend calls and arities used by both registration and translation.
// GoogleSQL preserves BigQuery's errors and nonfinite results, such as SIN(+inf) = NaN.
struct TrigonometricFunction {
  std::string_view name;
  const char* backend;
  duckdb_scalar_function_t implementation;
  std::size_t arity = 1;
};

constexpr auto kTrigonometricFunctions = std::to_array<TrigonometricFunction>({
    {.name = "SIN", .backend = "bq_sin", .implementation = Apply<fn::Sin<double>>},
    {.name = "COS", .backend = "bq_cos", .implementation = Apply<fn::Cos<double>>},
    {.name = "TAN", .backend = "bq_tan", .implementation = Apply<fn::Tan<double>>},
    {.name = "ASIN", .backend = "bq_asin", .implementation = Apply<fn::Asin<double>>},
    {.name = "ACOS", .backend = "bq_acos", .implementation = Apply<fn::Acos<double>>},
    {.name = "ATAN", .backend = "bq_atan", .implementation = Apply<fn::Atan<double>>},
    {
        .name = "ATAN2",
        .backend = "bq_atan2",
        .implementation = Apply<fn::Atan2<double>>,
        .arity = 2,
    },
    {.name = "ASINH", .backend = "bq_asinh", .implementation = Apply<fn::Asinh<double>>},
    {.name = "TANH", .backend = "bq_tanh", .implementation = Apply<fn::Tanh<double>>},
    {.name = "SINH", .backend = "bq_sinh", .implementation = Apply<fn::Sinh<double>>},
    {.name = "COSH", .backend = "bq_cosh", .implementation = Apply<fn::Cosh<double>>},
    {.name = "ACOSH", .backend = "bq_acosh", .implementation = Apply<fn::Acosh<double>>},
    {.name = "ATANH", .backend = "bq_atanh", .implementation = Apply<fn::Atanh<double>>},
    {.name = "CSC", .backend = "bq_csc", .implementation = Apply<fn::Csc<double>>},
    {.name = "SEC", .backend = "bq_sec", .implementation = Apply<fn::Sec<double>>},
    {.name = "COT", .backend = "bq_cot", .implementation = Apply<fn::Cot<double>>},
    {.name = "CSCH", .backend = "bq_csch", .implementation = Apply<fn::Csch<double>>},
    {.name = "SECH", .backend = "bq_sech", .implementation = Apply<fn::Sech<double>>},
    {.name = "COTH", .backend = "bq_coth", .implementation = Apply<fn::Coth<double>>},
});

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

// DuckDB keeps NUMERIC as DECIMAL(38, 9); see Codec<googlesql::NumericValue>.
constexpr uint8_t kNumericScale = 9;

// DuckDB has no NUMERIC or FLOAT64 generate_series overload. GoogleSQL supplies the
// generation and validation, with BigQuery's documented 1,048,576-element limit when
// `extended_limit` is set.
constexpr int kMaxGeneratedArraySize = 1048576;
template <typename T>
absl::Status GenerateArray(T start, T end, T step, bool extended_limit, std::vector<T>* out) {
  if (!extended_limit) {
    return fn::GenerateArray(start, end, step, out);
  }
  return fn::GenerateArrayHelper<fn::ArrayGenTrait<T, T>, kMaxGeneratedArraySize>(start, end, step,
                                                                                  out);
}

// Registers the NUMERIC functions, named as the FLOAT64 ones plus `suffix`.
template <typename T>
void RegisterDecimalMath(duckdb_connection connection, const std::string& suffix,
                         bool extended_array_limit) {
  LogicalType const type(duckdb_create_decimal_type(38, kNumericScale));
  for (const auto& [name, function] :
       std::initializer_list<std::pair<const char*, duckdb_scalar_function_t>>{
           {"bq_sqrt", Apply<fn::Sqrt<T>>},
           {"bq_cbrt", Apply<fn::Cbrt<T>>},
           {"bq_exp", Apply<fn::Exp<T>>},
           {"bq_ln", Apply<fn::NaturalLogarithm<T>>},
           {"bq_log10", Apply<fn::DecimalLogarithm<T>>},
       }) {
    Register(connection, (name + suffix).c_str(), {type.get()}, type.get(), function);
  }
  LogicalType const list(duckdb_create_list_type(type.get()));
  RegisterWithContext(connection, "bq_generate_array_numeric", {type.get(), type.get(), type.get()},
                      list.get(), ApplyWithContext<GenerateArray<T>>, extended_array_limit);
  // Spelled out: a braced pair of pointers would also match vector<duckdb_type>'s iterator range
  // constructor.
  const std::vector<duckdb_logical_type> two = {type.get(), type.get()};
  Register(connection, ("bq_pow" + suffix).c_str(), two, type.get(), Apply<fn::Pow<T>>);
  Register(connection, "bq_div_numeric", two, type.get(), Apply<fn::DivideToIntegralValue<T>>);
  Register(connection, "bq_multiply_numeric", two, type.get(), Apply<fn::Multiply<T>>);
  Register(connection, "bq_divide_numeric", two, type.get(), Apply<fn::Divide<T>>);
  Register(connection, "bq_safe_divide_numeric", two, type.get(),
           Apply<fn::Divide<T>, Errors::kNull>);
  Register(connection, ("bq_log" + suffix).c_str(), two, type.get(), Apply<fn::Logarithm<T>>);
  Register(connection, "bq_parse_numeric", {kVarchar}, type.get(), Apply<fn::ParseNumeric>);
}

}  // namespace

const std::unordered_map<std::string_view, FunctionDefinition<Rule>>& TrigonometricFunctions() {
  static const auto* const kFunctions = [] {
    auto* functions = new std::unordered_map<std::string_view, FunctionDefinition<Rule>>();
    std::ranges::transform(kTrigonometricFunctions, std::inserter(*functions, functions->end()),
                           [](const TrigonometricFunction& function) {
                             return std::pair{
                                 function.name,
                                 FunctionDefinition<Rule>{
                                     .rules = function_catalog::Same(
                                         function.backend, function.arity,
                                         {function_catalog::Is(1, {googlesql::TYPE_DOUBLE})}),
                                 },
                             };
                           });
    return functions;
  }();
  return *kFunctions;
}

void RegisterMathFunctions(duckdb_connection connection, const Options& options) {
  for (const auto& [name, function] :
       std::initializer_list<std::pair<const char*, duckdb_scalar_function_t>>{
           {"bq_sqrt", Apply<fn::Sqrt<double>>},
           {"bq_cbrt", Apply<fn::Cbrt<double>>},
           {"bq_exp", Apply<fn::Exp<double>>},
           {"bq_ln", Apply<fn::NaturalLogarithm<double>>},
           {"bq_log10", Apply<fn::DecimalLogarithm<double>>},
       }) {
    Register(connection, name, {kDouble}, kDouble, function);
  }
  for (const auto& function : kTrigonometricFunctions) {
    Register(connection, function.backend, std::vector<duckdb_type>(function.arity, kDouble),
             kDouble, function.implementation);
  }
  Register(connection, "bq_pow", {kDouble, kDouble}, kDouble, Apply<fn::Pow<double>>);
  Register(connection, "bq_log", {kDouble, kDouble}, kDouble, Apply<fn::Logarithm<double>>);
  LogicalType const float64(duckdb_create_logical_type(kDouble));
  LogicalType const list(duckdb_create_list_type(float64.get()));
  RegisterWithContext(connection, "bq_generate_array", {kDouble, kDouble, kDouble}, list.get(),
                      ApplyWithContext<GenerateArray<double>>, options.extended_array_limits);
  RegisterDecimalMath<googlesql::NumericValue>(connection, "_numeric",
                                               options.extended_array_limits);
  Register(connection, "bq_cosine_distance", {kAny, kAny}, kDouble, Distance<true>);
  Register(connection, "bq_euclidean_distance", {kAny, kAny}, kDouble, Distance<false>);
}

}  // namespace bigquery_emulator_duckdb::functions_duckdb
