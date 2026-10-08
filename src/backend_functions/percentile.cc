#include "googlesql/public/functions/percentile.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <type_traits>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "duckdb.h"
#include "googlesql/public/numeric_value.h"
#include "src/duckdb_handle.h"

// PERCENTILE_CONT and PERCENTILE_DISC. The translator collects each partition's values with
// list(), sorted as GoogleSQL sorts them: NULLs first, then NaNs, then the other values in
// ascending order. GoogleSQL's PercentileEvaluator then reads the result off the sorted list.

#include "src/backend_functions/percentile.h"
#include "src/backend_functions/register.h"
#include "src/backend_functions/scalar.h"

namespace bigquery_emulator_duckdb::backend_functions {
namespace {

using googlesql::NumericValue;

// The percentile of argument `column`, which has to be non-NULL.
template <typename T>
absl::StatusOr<googlesql::PercentileEvaluator<T>> Evaluator(const Arguments& arguments,
                                                            idx_t column,
                                                            const std::string& function) {
  if (arguments.IsNull(column)) {
    return absl::OutOfRangeError("The second argument to the function " + function +
                                 " must not be null");
  }
  if constexpr (std::is_same_v<T, double>) {
    return googlesql::PercentileEvaluator<double>::Create(arguments.Double(column));
  } else {
    const auto percentile = NumericValue::FromPackedInt(arguments.Decimal(column));
    if (!percentile.ok()) {
      return percentile.status();
    }
    return googlesql::PercentileEvaluator<NumericValue>::Create(*percentile);
  }
}

// A NUMERIC from the units of a DECIMAL(38, 9), whose range is NUMERIC's.
NumericValue Numeric(duckdb_hugeint units) {
  return NumericValue::FromPackedInt((static_cast<__int128>(units.upper) << 64) | units.lower)
      .value_or(NumericValue());
}

// PERCENTILE_CONT of the sorted list in argument 0, a list of DOUBLE or of DECIMAL(38, 9), at
// the percentile in argument 1. A NULL list has no values.
template <typename T>
void PercentileCont(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
  // DuckDB takes a DECIMAL(38, 9) as its units.
  using Result = std::conditional_t<std::is_same_v<T, double>, double, __int128>;
  EachRow(
      info, input, output,
      [](const Arguments& arguments) -> absl::StatusOr<std::optional<Result>> {
        const auto evaluator = Evaluator<T>(arguments, 1, "PERCENTILE_CONT");
        if (!evaluator.ok()) {
          return evaluator.status();
        }
        if (arguments.IsNull(0)) {
          return std::nullopt;
        }
        duckdb_vector list = arguments.Vector(0);
        const auto entry = VectorElement<duckdb_list_entry>(list, arguments.Row());
        duckdb_vector child = duckdb_list_vector_get_child(list);
        // The NULLs come first, so the first non-NULL element is found by bisection.
        uint64_t* validity = duckdb_vector_get_validity(child);
        std::size_t nulls = 0;
        for (std::size_t end = entry.length; validity != nullptr && nulls < end;) {
          const std::size_t middle = nulls + ((end - nulls) / 2);
          if (duckdb_validity_row_is_valid(validity, entry.offset + middle)) {
            end = middle;
          } else {
            nulls = middle + 1;
          }
        }
        T result{};
        bool found = false;
        if constexpr (std::is_same_v<T, double>) {
          const std::span values(
              static_cast<const double*>(duckdb_vector_get_data(child)) + entry.offset + nulls,
              entry.length - nulls);
          found = evaluator->template ComputePercentileCont<true>(values.begin(), values.end(),
                                                                  nulls, &result);
        } else {
          const auto values =
              std::span(static_cast<const duckdb_hugeint*>(duckdb_vector_get_data(child)) +
                            entry.offset + nulls,
                        entry.length - nulls) |
              std::views::transform(Numeric);
          found = evaluator->template ComputePercentileCont<true>(values.begin(), values.end(),
                                                                  nulls, &result);
        }
        if (!found) {
          return std::nullopt;
        }
        if constexpr (std::is_same_v<T, double>) {
          return result;
        } else {
          return result.as_packed_int();
        }
      },
      /*nulls=*/false);
}

// The position, counted from 1, of PERCENTILE_DISC in a sorted list of argument 0 elements, at
// the percentile in argument 1, a DOUBLE or a DECIMAL(38, 9); NULL for an empty list.
template <typename T>
void PercentileDiscPosition(duckdb_function_info info, duckdb_data_chunk input,
                            duckdb_vector output) {
  EachRow(
      info, input, output,
      [](const Arguments& arguments) -> absl::StatusOr<std::optional<int64_t>> {
        const auto evaluator = Evaluator<T>(arguments, 1, "PERCENTILE_DISC");
        if (!evaluator.ok()) {
          return evaluator.status();
        }
        const std::size_t length =
            arguments.IsNull(0) ? 0 : static_cast<std::size_t>(arguments.Int(0));
        // The list puts its NULLs first, so the position counts them as elements.
        const auto positions = std::views::iota(std::size_t{0}, length);
        const auto position = evaluator->template ComputePercentileDisc<std::size_t, true>(
            positions.begin(), positions.end(), 0);
        if (position == positions.end()) {
          return std::nullopt;
        }
        return static_cast<int64_t>(*position) + 1;
      },
      /*nulls=*/false);
}

}  // namespace

void RegisterPercentileFunctions(duckdb_connection connection) {
  LogicalType float64(duckdb_create_logical_type(kDouble));
  LogicalType float64_list(duckdb_create_list_type(float64.get()));
  LogicalType numeric(duckdb_create_decimal_type(38, 9));
  LogicalType numeric_list(duckdb_create_list_type(numeric.get()));
  LogicalType int64(duckdb_create_logical_type(kBigint));
  // Spelled out: a braced pair of pointers would also match vector<duckdb_type>'s iterator range
  // constructor.
  using Types = std::vector<duckdb_logical_type>;
  Register(connection, "bq_percentile_cont", Types{float64_list.get(), float64.get()},
           float64.get(), PercentileCont<double>, /*nulls=*/false);
  Register(connection, "bq_percentile_cont_numeric", Types{numeric_list.get(), numeric.get()},
           numeric.get(), PercentileCont<NumericValue>, /*nulls=*/false);
  Register(connection, "bq_percentile_disc_position", Types{int64.get(), float64.get()},
           int64.get(), PercentileDiscPosition<double>, /*nulls=*/false);
  Register(connection, "bq_percentile_disc_position_numeric", Types{int64.get(), numeric.get()},
           int64.get(), PercentileDiscPosition<NumericValue>, /*nulls=*/false);
}

}  // namespace bigquery_emulator_duckdb::backend_functions
