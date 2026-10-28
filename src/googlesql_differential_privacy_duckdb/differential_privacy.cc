#include "src/googlesql_differential_privacy_duckdb/differential_privacy.h"

#include <cstdint>
#include <optional>
#include <span>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "absl/status/statusor.h"
#include "duckdb.h"
#include "src/duckdb_functions/register.h"
#include "src/duckdb_functions/scalar.h"
#include "src/duckdb_handle.h"
#include "src/googlesql_differential_privacy/differential_privacy.h"

namespace bigquery_emulator_duckdb::differential_privacy_duckdb {
namespace {

using namespace duckdb_functions;
using googlesql_differential_privacy::Parameters;

template <typename T>
T Value(const Arguments& arguments, idx_t column) {
  if constexpr (std::is_same_v<T, double>) {
    return arguments.Double(column);
  } else {
    return arguments.Int(column);
  }
}

// The list of argument 0, which the translator builds from the non-NULL contributions of a group,
// and is NULL for a group without any.
template <typename T>
std::vector<T> Values(const Arguments& arguments) {
  if (arguments.IsNull(0)) {
    return {};
  }
  duckdb_vector vector = arguments.Vector(0);
  const auto entry = VectorElement<duckdb_list_entry>(vector, arguments.Row());
  const auto* values =
      static_cast<T*>(duckdb_vector_get_data(duckdb_list_vector_get_child(vector)));
  return {values + entry.offset, values + entry.offset + entry.length};
}

// Arguments 1 to 4: the lower and upper contribution bounds, NULL for implicit bounds, epsilon,
// and max_groups_contributed, NULL for none.
template <typename T>
Parameters<T> ParametersOf(const Arguments& arguments) {
  Parameters<T> parameters{
      .epsilon = arguments.Double(3),
      .max_groups_contributed = std::nullopt,
      .bounds = std::nullopt,
  };
  if (!arguments.IsNull(4)) {
    parameters.max_groups_contributed = arguments.Int(4);
  }
  if (!arguments.IsNull(1)) {
    parameters.bounds = std::pair(Value<T>(arguments, 1), Value<T>(arguments, 2));
  }
  return parameters;
}

template <typename T>
absl::StatusOr<T> Sum(const Arguments& arguments) {
  const std::vector<T> values = Values<T>(arguments);
  return googlesql_differential_privacy::Sum<T>(std::span<const T>(values),
                                                ParametersOf<T>(arguments));
}

absl::StatusOr<double> Mean(const Arguments& arguments) {
  const std::vector<double> values = Values<double>(arguments);
  return googlesql_differential_privacy::Mean(values, ParametersOf<double>(arguments));
}

}  // namespace

void RegisterDifferentialPrivacyFunctions(duckdb_connection connection) {
  // Each call adds noise of its own, so DuckDB must neither fold nor share calls.
  for (const auto& [name, type, function] : {
           std::tuple{"bq_dp_sum_int64", kBigint, EachRow<Sum<int64_t>, false>},
           std::tuple{"bq_dp_sum_float64", kDouble, EachRow<Sum<double>, false>},
           std::tuple{"bq_dp_avg", kDouble, EachRow<Mean, false>},
       }) {
    LogicalType const element(duckdb_create_logical_type(type));
    LogicalType const list(duckdb_create_list_type(element.get()));
    LogicalType const bigint(duckdb_create_logical_type(kBigint));
    LogicalType const float64(duckdb_create_logical_type(kDouble));
    Register(connection, name,
             {list.get(), element.get(), element.get(), float64.get(), bigint.get()}, element.get(),
             function, /*nulls=*/false, std::nullopt, /*volatile_result=*/true);
  }
}

}  // namespace bigquery_emulator_duckdb::differential_privacy_duckdb
