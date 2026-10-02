#include "googlesql/public/functions/math.h"

#include <initializer_list>
#include <utility>

#include "absl/status/status.h"
#include "duckdb.h"
#include "src/backend_functions/internal.h"

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
}

}  // namespace bigquery_emulator_duckdb::backend_functions
